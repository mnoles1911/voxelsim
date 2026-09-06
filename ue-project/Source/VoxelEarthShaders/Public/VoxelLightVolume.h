// VoxelLightVolume.h -- THE GPU-PROPAGATED SUNLIGHT VOLUME (Phase L3).
//
// docs/vs-lighting-implementation-plan-2026-09-06.md phase L3, which is the
// headline of that plan; the mechanism it reproduces is documented in
// docs/vintage-story-lighting-research-2026-09-05.md sections 1.2 and 2 (item
// 2, "light arrives by propagation, so it wraps into shade").
//
// ===========================================================================
// DISPLAY ONLY. ADR-0006 INVARIANTS 3-5, STATED AT THE CLASS AND MEANT.
// ===========================================================================
//
// NOTHING IN THE GAME MAY EVER READ THIS VOLUME. It is per-client cosmetic
// state: it is built from whatever bricks happen to be resident on THIS
// machine, at THIS camera position, over however many frames the relax budget
// took to settle -- so two clients standing in the same cave hold different
// numbers in it, legitimately and by construction. A gameplay system that
// asked "how dark is it here" through this volume would be asking a question
// whose answer differs per machine, which is the determinism boundary this
// project spends most of its doctrine defending (the F7 boundary doc's rule,
// and the identical argument VoxelLightField.h:8-12 and VoxelSkySubsystem.h:
// 18-24 make for the GI field and the sky rig).
//
// The enforcement is structural rather than a comment: this header lives in
// VoxelEarthShaders, VoxelEarth may not be depended on from here, there is no
// sampler that returns a level to the CPU, and the only readback in the file is
// a census counter. A future "is the player in shadow" feature does NOT get to
// read this -- it gets its own deterministic query against the voxel data.
//
// ===========================================================================
// WHAT IT IS
// ===========================================================================
//
// A camera-following 3D texture of SKY LIGHT LEVELS -- how much of the open sky
// reaches a point -- seeded from the brick pool's own occupancy and then
// diffused with Vintage Story's rule: level = max(level, maxNeighbour -
// absorption). That is a flood fill, moved onto the GPU and amortized, and it
// is the term the marcher has never had: today's voxel.March.AmbientIntensity
// lifts the inside of a cave exactly as much as the outside of a mountain (its
// own cvar comment says so), because a flat constant cannot know the
// difference. This volume is what knows the difference.
//
//   Cells        128 x 128 x 64 at 100 UU (1.0 m), i.e. a 128 x 128 x 64 m box
//                around the camera. 1 m matches VS's own granularity: their
//                field is one level per BLOCK, and propagating at our 25 mm
//                craft lattice would be 64,000x the cells to describe the same
//                gradient.
//   Format       PF_R8G8B8A8, and PING-PONGED, so 128*128*64*4 = 4.0 MiB per
//                copy and 8.0 MiB resident for the pair. See the format note
//                below for why 8 bits/cell (the plan's v1 sketch) is not
//                reachable and what was traded for what.
//   Reach        the near field only. Beyond the box the marcher falls back to
//                the flat ambient, which is L1+L2's bounded response -- a
//                distance-independent floor, so the seam is a change of
//                GRADIENT rather than a change of level and is soft by
//                construction. Same call VS makes with its near-field-only
//                shadow cascades (research doc section 1.4).
//
// FORMAT: 32 BITS PER CELL, NOT THE PLAN'S 8, AND THE REASON IS THE SAMPLER.
// The whole argument for a texture rather than a buffer is that hardware
// trilinear filtering IS the per-vertex smoothing VS builds by hand (research
// doc section 3, row 3) -- one SampleLevel and the marcher gets the
// within-face gradient for free. A filtered value is only meaningful if every
// bit of the channel is part of one number, so a packed "6-bit level + 2 spare
// bits" byte cannot be filtered at all: the filter would blend the spare bits
// into the level. So the level gets a whole channel, and L4's block light gets
// its own channels rather than the leftover bits:
//
//   R  sky light level, 0..1 (0 = no sky reaches here, 1 = open sky)
//   G  AIR MASK: 1 where the seed pass found air, 0 where it found solid.
//      This is what stops the relax from flooding light through a wall, and it
//      is also the marcher's validity signal for a sample that lands inside
//      rock. Cached by the seed pass rather than re-probed per relax
//      iteration -- a per-cell brick probe is a chunk-index load plus a brick
//      load, and paying that 1,048,576 times per iteration is the difference
//      between this fitting the budget and not.
//   B  RESERVED for L4 (block-light level). Written as 0 today, and the
//      marcher does not read it.
//   A  RESERVED for L4 (block-light hue index / saturation). Written as 1
//      today so a debug view of the volume is not invisible.
//
// ===========================================================================
// THE PASSES, AND THE DISPATCH SHAPES THE BUDGET IS ARGUED FROM
// ===========================================================================
//
// SEED  [cs, 64 threads/group, one thread per COLUMN]
//   One thread walks one (x, y) column from the top of the box down, carrying
//   "is the sky still visible". Above the first solid cell: air, level 1. At
//   and below it: air cells get level 0 (the relax fills them from the sides),
//   solid cells get level 0 and airMask 0. 64 cells of serial work per thread.
//   Dispatched over a COLUMN LIST, so a frame that dirtied 40 chunks seeds ~640
//   columns rather than all 16,384.
//
// RELAX [cs, 8x8x4 = 256 threads/group, one thread per CELL]
//   Dst = airMask ? max(Src, maxOf6Neighbours - Absorption) : 0.
//   16 x 16 x 16 = 4,096 groups over the whole volume. Whole-volume rather
//   than dirty-region, deliberately: at 4 MiB the read set fits comfortably in
//   L2 on any GPU this project targets, so the pass is dominated by dispatch
//   and cache behaviour rather than by how many cells actually changed, and a
//   compacted dirty list would cost more to build than it saves. THE DIRTY
//   STATE STILL DECIDES WHETHER THE PASS RUNS AT ALL and how many iterations
//   it gets, which is where the amortization actually comes from: a settled
//   camera in a settled world runs ZERO iterations and the volume costs
//   nothing but its memory.
//
// BUDGET (<= 1.0 ms amortized, the plan's target). Per relax iteration the
// kernel reads 7 texels and writes 1 per cell: 1,048,576 cells x 32 B of reads
// = 33 MB of ADDRESSED reads, but the 6 neighbours of adjacent cells are the
// same texels, so the real traffic is close to the 4 MB working set plus 4 MB
// written. At the owner's box (RX 7800 XT, ~620 GB/s) two iterations is
// ~0.03 ms of bandwidth; the honest number is dominated by latency and
// dispatch, which is exactly why it must be MEASURED and is not claimed here.
// THIS FILE MEASURES NOTHING -- the leg does (see the plan's execution log).
//
// ===========================================================================
// WHAT IT IS NOT, v1
// ===========================================================================
//
// * NOT A SHADOW. It propagates SKY visibility, not the sun's direction. A
//   south-facing overhang and a north-facing one at the same depth get the
//   same level. The DIRECTIONAL part of the answer is L1's bounded wrap and
//   the shadow march; this supplies the SPATIAL part, which is the part
//   nothing in this renderer has ever had.
// * NOT BOUNDED BY THE WORLD, only by the box. The seed assumes open sky above
//   the volume's top face, so a mountain 40 m above the camera does not shade
//   the ground under it until the camera climbs enough to bring it inside.
//   VS has the same property with its chunk columns and it reads as "the light
//   settles in" rather than as a defect.
// * NOT COLOURED. One sky level, tinted by L2's published AmbientColour at the
//   consumption site. Coloured block light is L4.
// * DEFAULT OFF (voxel.Light.Propagated 0). Nothing is allocated, no pass is
//   added, the marcher's sample is compiled out of the uniform's Enabled test,
//   and the frame is byte-identical to the pre-L3 renderer.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "RHIResources.h"
#include "RenderGraphFwd.h"
#include "ShaderParameters.h"

class FRDGBuilder;
class FGlobalShaderMap;
struct FVoxelBrickIndexDelta;

// The marcher's binding. A GLOBAL uniform buffer, registered shader-side as
// "VoxelLightVol", exactly like FVoxelGIVolumeParameters is registered as
// "VoxelGIVol" -- so in HLSL these are reached as VoxelLightVol.Volume and
// NEVER as a loose `Texture3D Volume;`, which compiles perfectly and then reads
// zeros forever because it is a different, unbound symbol. Same trap, same
// wording, because it has caught this project twice.
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelLightVolumeParameters, VOXELEARTHSHADERS_API)
	// R = sky level, G = air mask, BA reserved for L4. See the header block.
	SHADER_PARAMETER_TEXTURE(Texture3D, Volume)
	SHADER_PARAMETER_SAMPLER(SamplerState, VolumeSampler)
	// The volume's min corner RELATIVE TO THE CAMERA, in UU, and the reciprocal
	// of its full size per axis.
	//
	// CAMERA-RELATIVE FOR THE REASON FVoxelGIVolumeParameters STATES AT LENGTH:
	// at ~8.4M UU a float32 ulp is 1.0 UU against a 100 UU cell, so computing
	// WorldPos - VolumeOrigin at world scale is catastrophic cancellation. The
	// marcher's hit position is already camera-relative (VoxelMarch.usf's HitTWS)
	// and this volume is 128 m across, so both operands are small and the whole
	// lookup stays where float32 has headroom.
	SHADER_PARAMETER(FVector3f, OriginRelCameraUU)
	SHADER_PARAMETER(FVector3f, InvSizeUU)
	// One cell, in UU. The marcher pushes its sample OFF the surface by a
	// fraction of this before sampling -- a sample taken exactly on a face lands
	// half inside the solid cell it belongs to, whose level is 0, and the whole
	// wall would read as unlit. Same push-out the GI sample makes, same reason.
	SHADER_PARAMETER(float, CellSizeUU)
	// voxel.Light.Strength: how much of the ambient/wrap share the sampled level
	// is allowed to scale. 0 = sampled and ignored (a bisection state: proves the
	// bind and the pass without changing a pixel), 1 = full.
	SHADER_PARAMETER(float, Strength)
	// The floor the scale may not fall below, so a cell the propagation has not
	// reached yet does not read as a hole while the volume settles. This is the
	// same guarantee VS's MINBRIGHT gives ("Light up all caves", research doc
	// section 1.4) and it is what makes a half-settled volume acceptable to look
	// at rather than a black flicker.
	SHADER_PARAMETER(float, Floor)
	// 0 = the marcher does not sample at all and its emissive is byte-identical
	// to the pre-L3 expression. Off is a uniform read, not a permutation, for the
	// same reason the GI volume's Enabled is: it must be flippable live inside
	// one session or its own A/B cannot be taken.
	SHADER_PARAMETER(uint32, Enabled)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

namespace VoxelLightVolume
{
	// Cells per axis. STRUCTURAL CONSTANTS, not cvars: they size an allocation
	// that happens once, they are mirrored as shader defines (the seed kernel's
	// column walk needs the Z extent at compile time), and a live change would
	// mean a live re-allocation of a texture the marcher is sampling. The plan's
	// escape hatch -- a Lighting Quality row mapping reach to 0/100/200 m -- is a
	// LATER change that swaps these for a startup-only cvar, and it is not
	// pretended to exist today.
	inline constexpr int32 kDimXY = 128;
	inline constexpr int32 kDimZ = 64;
	// One cell in UU. 100 UU = 1.0 m; see the header block for why not 25 mm.
	inline constexpr float kCellSizeUU = 100.0f;
	// Bytes of VRAM the pair costs, spelled out so the number in the log line and
	// the number in the plan cannot drift from the number actually allocated.
	inline constexpr int64 kBytesPerVolume = int64(kDimXY) * kDimXY * kDimZ * 4;

	// voxel.Light.Propagated. THE MASTER, DEFAULT 0. With it off nothing is
	// allocated, no pass is added and the marcher does not sample.
	VOXELEARTHSHADERS_API bool IsEnabled();
}

// One frame's worth of everything the update needs, copied from the SAME
// authorities the march dispatch binds -- the FVoxelMarchBoundInputs pattern,
// and for its stated reason: FVoxelMarchViewParameters is private to
// VoxelMarchRenderer.cpp, so a POD filled member-from-member at ONE call site
// adjacent to the march's own fill keeps a single authority without exporting
// shader-parameter machinery through a public header.
struct FVoxelLightVolumeFrame
{
	// The camera, in WORLD UU and in DOUBLE. Not the camera voxel: a level-0
	// voxel is 10 UU and this volume's cell is 100 UU, so rounding the camera to
	// a voxel would move the sampled volume by up to a tenth of a cell -- which
	// is a visible slide of the whole lighting field under a stationary player.
	FVector CameraWorldUU = FVector::ZeroVector;
	// The march frame's origin in level-0 world voxels (MarchBrickOriginVoxel),
	// bound to the seed kernel so its brick probes address the same lattice the
	// marcher's own walk does.
	FIntVector FrameOriginVoxel = FIntVector::ZeroValue;
	// THE CHUNK INDEX SRV THE CALLER ALREADY MADE, handed over rather than
	// registered here, and this is a correctness requirement rather than a
	// convenience. FVoxelMarchChunkIndex::Register CONSUMES the frame's staged
	// upload and clears its valid flag, so a second call inside one frame takes
	// the upload for whoever called first and leaves the other reader on the
	// previous frame's pooled buffer. This feature must never be the thing that
	// makes the marcher's own index one frame stale.
	//
	// Null is legal and means the index has not uploaded yet (the first frames of
	// every run); the update then declines its seed and counts it.
	FRDGBufferSRVRef ChunkIndexSRV = nullptr;
	// The traversal step budget, bound because VoxelBrickTraverse.ush declares it
	// and the parameter map therefore requires it. The seed pass takes no walks,
	// so its value cannot change any output.
	int32 StepBudget = 64;
};

// Adds this frame's seed and relax passes and refreshes the marcher's uniform
// buffer. RENDER THREAD, inside the marcher's graph, called immediately before
// the march pass.
//
// DOES NOTHING AND SAYS NOTHING when voxel.Light.Propagated is 0: no
// allocation, no pass, and the uniform buffer keeps its Enabled = 0 form, which
// is what makes the off arm byte-identical rather than merely cheap.
//
// EVERY DECLINE IS COUNTED, NOT SILENT -- the pool never flushed, the index has
// not uploaded, the volume could not be allocated. The census line prints the
// reason, because "the arm is on and did nothing" and "the arm is off" are
// different findings and this project has published the first as the second.
VOXELEARTHSHADERS_API void VoxelLightVolumeUpdate_RenderThread(
	FRDGBuilder& GraphBuilder, const FGlobalShaderMap* ShaderMap,
	const FVoxelLightVolumeFrame& Frame);

// The marcher's binding. NEVER NULL -- an unbound member of a uniform buffer is
// a validation failure, not a tolerated no-op, so with the feature off this
// returns a buffer pointing at GBlackVolumeTexture with Enabled = 0 (the GI
// volume's rule, verbatim).
//
// THE REF, NOT THE RAW POINTER, and that is not a style choice: assigning a
// SHADER_PARAMETER_STRUCT_REF member from a raw FRHIUniformBuffer* through
// TUniformBufferRef<T>'s constructor COMPILES and then produces an UNSET
// binding, which UE reports as a fatal "required shader parameter was not set"
// at the first dispatch. FVoxelGIVolume::GetUniformBufferRef carries the same
// warning and records the hour it cost on 2026-08-21.
//
// RENDER THREAD. Creates the off binding on first use if no update has run.
VOXELEARTHSHADERS_API const TUniformBufferRef<FVoxelLightVolumeParameters>&
VoxelLightVolumeGetUniformBuffer();

// ---------------------------------------------------------------------------
// THE DIRTY CHANNEL -- AND IT IS THE POOL'S OWN, NOT A SECOND ONE
// ---------------------------------------------------------------------------
//
// FVoxelBrickIndexDelta is what the brick pool already publishes at the end of
// every Flush: the chunks that became resident and the chunks that were
// retired, on the game thread, in the batch that flush enqueued. That IS the
// upload path's dirty knowledge -- FVoxelMarchChunkIndex consumes exactly this
// delta to keep the marcher's lookup grid current -- so this hook rides it
// rather than inventing a parallel notification with its own ordering rules and
// its own way of being wrong.
//
// IT IS CALLED FROM FVoxelMarchChunkIndex::ApplyDelta rather than from a second
// sink on the pool, because FVoxelBrickPool::SetIndexSink holds exactly ONE
// sink and the chunk index owns it. Chaining a second subscriber onto that seam
// would make the pool's "the delta is delivered AFTER the pool's own render
// command" ordering guarantee depend on subscriber order; one call site inside
// the existing consumer has no such question.
//
// GAME THREAD. Cheap and unconditional: it returns immediately unless the
// feature is on, and when it is on it does a level test and a bounded array
// append per entry. Level-0 entries only -- this volume covers 64 m of radius,
// which is inside ring 0 (0-128 m) by construction, so a level-1 chunk cannot
// intersect it and a cover chunk is not terrain. Non-level-0 entries are
// COUNTED as dropped rather than silently ignored, so the census can say "the
// world is streaming and none of it was ours" instead of showing a zero that
// looks like a dead wire.
//
// OVERFLOW FORCES A FULL RESEED, which is the safe direction: a cold fill
// delivers tens of thousands of chunks in one flush and queueing them all would
// be a megabyte of coordinates to describe "everything changed". The overflow
// count is printed, because a run that is permanently overflowing is running
// the expensive path every frame and should be visible as such.
VOXELEARTHSHADERS_API void VoxelLightVolumeNoteBrickIndexDelta_GameThread(
	const FVoxelBrickIndexDelta& Delta);

// ---------------------------------------------------------------------------
// The engagement census (the house 1 Hz line)
// ---------------------------------------------------------------------------
//
// EVERY FIELD IS A COUNT OF SOMETHING DISPATCHED OR DECIDED, never a derived
// ratio, and the printer divides. Two of them are honest about what they are
// not: CellsRelaxed and SeedColumns are what the HOST DISPATCHED, not what the
// GPU confirmed -- there is no readback here by design (a readback ring for a
// scaffold that ships off would be machinery measuring a pass nobody runs). A
// leg reading zero on a moving camera therefore proves the host never
// dispatched, which is the failure worth catching; a leg reading non-zero
// proves the dispatch happened and the owner's frame proves the rest.
struct FVoxelLightVolumeCensus
{
	uint64 CellsRelaxed = 0;     // cells x relax iterations dispatched
	uint64 SeedColumns = 0;      // columns dispatched into the seed pass
	uint64 Recentres = 0;        // camera left the dead zone -> full reseed
	uint64 FullReseeds = 0;      // recentres plus dirty-queue overflows
	uint64 DirtyChunks = 0;      // level-0 index-delta entries accepted
	uint64 DirtyDropped = 0;     // entries at a level this volume does not carry
	uint64 Overflows = 0;        // dirty queue full -> forced full reseed
	uint64 Frames = 0;           // frames the update ran at all
	uint64 DeclinedNoPool = 0;   // brick pool has never flushed
	uint64 DeclinedNoIndex = 0;  // chunk index has never uploaded
	uint64 DeclinedNoVolume = 0; // allocation failed or was refused
	int32 DirtyDepth = 0;        // queue depth right now (not a window sum)
	FIntVector OriginVoxel = FIntVector::ZeroValue; // the volume's min corner, level-0 voxels
	bool bArmed = false;         // from the CVAR, never from whether anything ran
};

// Drain-and-reset, the GetAndReset pattern every window in this module uses.
// Any thread. Zeros with Frames == 0 mean "the update never ran", which is NOT
// the same finding as "it ran and relaxed nothing" -- bArmed is what separates
// them and it comes from the cvar.
VOXELEARTHSHADERS_API FVoxelLightVolumeCensus VoxelLightVolumeGetAndResetCensus();
