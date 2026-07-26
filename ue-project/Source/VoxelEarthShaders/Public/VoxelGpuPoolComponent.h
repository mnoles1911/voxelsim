// Many chunks, ONE primitive, ONE draw call (ADR-0006, G2/G3).
//
// This is the component the whole ADR exists for. UVoxelGpuChunkComponent
// proves a chunk can be drawn from GPU-resident quads, but it is one primitive
// per chunk -- which leaves the FScene::AddPrimitive funnel that ADR-0006
// measured as the frame-time ceiling exactly where it was.
//
// Here every resident chunk lives in one shared quad buffer, every quad names
// its owning chunk, and the vertex shader looks up that chunk's origin. So a
// single draw covers the entire pool, and streaming a chunk in or out writes
// into buffers and changes a triangle count -- it never touches the renderer's
// primitive list.
//
// Deliberately NOT yet wired to the streaming cascade: this proves the draw
// shape first. G3 replaces the append-only upload here with the tested
// FVoxelGpuGeometryPool suballocator and drives it from the desired set.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "RHIResources.h"
#include "VoxelGpuGeometryPool.h"
#include "VoxelGpuPoolComponent.generated.h"

// The pool's two big GPU buffers, owned by the COMPONENT rather than the scene
// proxy (Wave D / D1).
//
// WHY THIS EXISTS, AND WHAT IT FIXES. These used to be created inside
// FVoxelGpuPoolSceneProxy::CreateRenderThreadResources and destroyed with the
// proxy, and CreateSceneProxy re-uploaded the whole CPU shadow every time a new
// proxy was built. That is correct only while the CPU shadow is the sole author
// of the quads. Once the GPU writes into the pool directly, any of the four
// MarkRenderStateDirty paths throws the proxy away and re-uploads stale CPU
// content over GPU-written geometry -- which presents as TERRAIN REVERTING TO
// OLDER GEOMETRY AFTER AN UNRELATED EVENT, the hardest failure in this wave to
// trace back to its cause.
//
// The fix is one level of ownership, not an immortal vertex factory. The
// factory bakes these SRVs into a uniform buffer in InitRHI, but that uniform
// buffer holds nothing else of substance -- so a new proxy simply builds a
// fresh one pointing at the SAME SRVs. The quads survive because the buffer was
// never destroyed and never re-uploaded.
//
// Held by shared pointer so the last reference can be dropped on the render
// thread (see UVoxelGpuPoolComponent::BeginDestroy) rather than wherever the
// component happens to be collected.
struct FVoxelGpuPoolBuffers
{
	FBufferRHIRef QuadBuffer;
	FBufferRHIRef ChunkIdBuffer;

	FShaderResourceViewRHIRef QuadSRV;
	FShaderResourceViewRHIRef ChunkIdSRV;

	// D1: the GPU-write half. Created alongside the SRVs so a compute pass can
	// write quads in place. Nothing dispatches through these yet -- they are the
	// buffers the mesher will target once D4 wires it up.
	FUnorderedAccessViewRHIRef QuadUAV;
	FUnorderedAccessViewRHIRef ChunkIdUAV;

	// Elements the buffers were created with. The whole capacity is allocated up
	// front because writes address ABSOLUTE pool offsets; if a later proxy wants
	// more than this, the buffers must be rebuilt (and the GPU content is
	// genuinely gone, so that path re-uploads from the CPU shadow).
	int32 CapacityQuads = 0;

	bool IsValid() const { return QuadBuffer.IsValid() && ChunkIdBuffer.IsValid(); }
};

using FVoxelGpuPoolBuffersRef = TSharedPtr<FVoxelGpuPoolBuffers, ESPMode::ThreadSafe>;

// One contiguous run of quads to write into the GPU buffer. First is the
// destination offset in the pool; SrcOffset indexes the flat staging array the
// render command carries, since several runs travel in one payload.
struct FVoxelQuadUploadRun
{
	uint32 First = 0;
	uint32 Count = 0;
	uint32 SrcOffset = 0;
};

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class VOXELEARTHSHADERS_API UVoxelGpuPoolComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelGpuPoolComponent();

	// Sets the pool's capacity in quads. Must be called before the first
	// AddChunk. For scale: the whole 2 km cascade measured 9,441,170 quads.
	void InitPool(uint32 CapacityQuads);

	// One resident chunk's contiguous span of the quad pool, plus the chunk-table
	// entry that describes where it is in space.
	//
	// Exists so the scene proxy can FRUSTUM-CULL. The pool's whole design is one
	// primitive and one draw covering every resident quad, and the measured cost
	// of that is a renderer whose frame time does not depend on what is on
	// screen: pointing the camera at almost nothing left it unchanged (18.58 ->
	// 19.05 ms) while the per-chunk component path got 64% cheaper. Culling needs
	// to know, per chunk, which quads are its own -- which the component knows
	// from its allocations and the proxy cannot recover, because the proxy's
	// copy of the per-quad chunk id array is not kept current by the incremental
	// upload path (that writes the GPU buffer only).
	struct FChunkRun
	{
		uint32 ChunkId = 0;
		uint32 FirstQuad = 0;
		uint32 NumQuads = 0;
	};

	// A surface height that can never gate anything off. Used for the hidden
	// chunk and by callers with no world subsystem to sample, mirroring
	// BuildChunkVertexData's own "no subsystem -> always surface" fallback.
	// -1e30 rather than -FLT_MAX so the shader's `- kSurfaceBandUU` stays finite.
	static constexpr float kNoSurfaceGate = -1.0e30f;

	// Adds one chunk's quads at a component-space origin. Quads are expected in
	// the GPU mesher's packed form, already re-based out of brick-local coords.
	// Returns a handle, or INDEX_NONE if the pool has no contiguous room.
	//
	// Params carries the per-chunk shading inputs the 8-byte quad packing has no
	// room for -- climate in xy, surface height in z, one spare in w. See
	// FVoxelQuadVertexFactoryParameters::ChunkParams for the exact layout and
	// the reason z is chunk-relative.
	int32 AddChunk(const TArray<uint64>& InQuads, const FVector3f& OriginUU, int32 Level,
	               const FVector4f& Params = FVector4f(0.5f, 0.5f, kNoSurfaceGate, 0.0f));

	// Releases a chunk's range back to the pool.
	//
	// The quads are NOT removed from the buffer -- one draw covers the whole
	// pool, so a freed range would otherwise keep rendering stale geometry.
	// Instead its quads are pointed at the reserved hidden chunk, whose scale
	// is zero, which collapses every vertex to a point. They cost vertex
	// invocations until the range is reused, and produce no pixels.
	void RemoveChunk(int32 Handle);

	// Re-meshes a resident chunk in place.
	//
	// Edits are the reason this exists. Implementing a re-mesh as
	// RemoveChunk + AddChunk would fragment the pool worst on exactly the
	// chunks that re-mesh most -- the ones being actively dug. When the new
	// quad count still fits the existing allocation the range is reused; only
	// a chunk that outgrew its slot falls back to reallocating.
	//
	// Returns the handle (possibly a new one if it had to reallocate), or
	// INDEX_NONE if there was no room.
	int32 UpdateChunk(int32 Handle, const TArray<uint64>& InQuads);

	// Drops every chunk.
	void ClearChunks();

	int32 GetNumChunks() const { return NumLiveChunks; }
	int32 GetNumQuads() const { return PooledQuads.Num(); }
	uint32 GetHighWaterMarkQuads() const { return Pool.GetHighWaterMark(); }
	uint32 GetFreeQuads() const { return Pool.GetFreeQuads(); }
	uint32 GetLargestFreeRun() const { return Pool.GetLargestFreeRun(); }
	// 1 means unfragmented. Rising while GetFreeQuads() stays healthy is the
	// early warning that allocations are about to start failing on a pool that
	// still looks half empty.
	int32 GetFreeRunCount() const { return Pool.GetFreeRunCount(); }

	// Cumulative count of AddChunk/UpdateChunk calls that found no room, and the
	// quads they wanted. NON-ZERO MEANS GEOMETRY IS MISSING FROM THE WORLD.
	//
	// This is a counter rather than only a log line because the failure is
	// otherwise invisible in every aggregate: a chunk that fails to allocate is
	// simply not added, so liveChunks, highWater and the resident quad count all
	// stay self-consistently smaller and nothing looks wrong. A cascade that
	// overflowed the pool and dropped a third of its chunks reads as a cheaper,
	// faster run -- i.e. exactly like a success. Wave F moves R0 to 128 m against
	// a pool already at ~93% of capacity, so this is the difference between a
	// result and an artefact.
	int64 GetAllocFailureCount() const { return AllocFailureCount; }
	int64 GetAllocFailureQuads() const { return AllocFailureQuads; }

	void SetChunkMaterial(UMaterialInterface* InMaterial);
	UMaterialInterface* GetChunkMaterialOrDefault() const;

	// Names this pool in every log line it emits. There is more than one pool
	// instance now (terrain under voxel.Stream.GPU, water under
	// voxel.Water.GPU), and a pooled primitive is diagnosed almost entirely
	// from its log lines -- two pools both printing "VoxelGpuPool draw
	// SUBMITTED" would make the one number that matters unattributable.
	// Must be set BEFORE the proxy is created; the proxy takes a copy.
	void SetPoolName(const FString& InName) { PoolName = InName; }

	// Edge length, in voxels, of one entry in this pool -- 32 for a terrain
	// render chunk, 8 for a vxc::WaterBrick8. Used only to grow LocalBounds by
	// the right amount per AddChunk. Getting it too LARGE is merely a looser
	// cull; too small culls away geometry that is genuinely there, so the
	// default stays at terrain's 32.
	void SetChunkEdgeVoxels(int32 InEdgeVoxels) { ChunkEdgeVoxels = FMath::Max(1, InEdgeVoxels); }
	// The proxy's frustum cull needs the same extent AddChunk grows the bounds
	// by, so that a culled chunk can never be tighter than the bounds the scene
	// already tested the whole primitive against.
	int32 GetChunkEdgeVoxels() const { return ChunkEdgeVoxels; }

	// Floor on the chunk table's GPU allocation, in entries. Exceeding whatever
	// the proxy was built with is not an error but it IS a full render-state
	// rebuild (PushUpdatesToProxy falls back to MarkRenderStateDirty), which
	// re-uploads the entire quad buffer. Terrain grows past its floor a handful
	// of times while the cascade fills and then stops; a pool whose resident
	// count oscillates around the floor would pay that repeatedly, so it wants a
	// floor above its own steady state. Must be set before the proxy is created.
	void SetChunkTableCapacity(int32 InEntries) { ChunkTableCapacity = FMath::Max(1, InEntries); }

	//~ UPrimitiveComponent
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials,
	                              bool bGetDebugMaterials = false) const override;
	virtual void DestroyRenderState_Concurrent() override;
	//~ End UPrimitiveComponent

	//~ Begin UObject
	// Drops the persistent buffers' last reference on the RENDER thread. Not
	// DestroyRenderState_Concurrent -- that fires on every MarkRenderStateDirty,
	// which is precisely the event these buffers exist to survive.
	virtual void BeginDestroy() override;
	//~ End UObject

	// DESTRUCTIVE DEBUG — see voxel.Stream.PoolClobberTest. Corrupts the first N
	// quads of the CPU shadow and forces proxy recreation, so that "rebound" and
	// "re-uploaded" predict different pictures instead of being indistinguishable.
	void DebugClobberShadowAndRecreate(int32 NumQuadsToClobber);

private:
	// See FVoxelGpuPoolBuffers. The holder is allocated on demand; the buffers
	// inside it are created on the render thread by the FIRST proxy and reused
	// by every proxy after it.
	FVoxelGpuPoolBuffersRef PoolBuffers;
	FVoxelGpuPoolBuffersRef GetOrCreatePoolBuffers();

	// One flat buffer for every chunk's quads, in insertion order.
	TArray<uint64> PooledQuads;

	// Parallel to PooledQuads: which chunk each quad belongs to. This is what
	// removes per-draw state and lets one draw span the pool.
	TArray<uint32> QuadChunkIds;

	// xyz = chunk origin in component space (unreal units), w = mip scale.
	TArray<FVector4f> ChunkOrigins;

	// Parallel to ChunkOrigins, indexed by the same chunk id. See
	// FVoxelQuadVertexFactoryParameters::ChunkParams for the channel layout.
	TArray<FVector4f> ChunkParams;

	// Chunk id 0 is RESERVED as the hidden chunk: origin (0,0,0), scale 0.
	// Freed quads point at it and collapse to a degenerate point.
	static constexpr uint32 kHiddenChunkId = 0;

	// Table entries whose chunk was removed, available for the next AddChunk.
	//
	// Without this the chunk table only ever GREW: AddChunk appended a fresh
	// entry every time and RemoveChunk gave nothing back, so the table tracked
	// "chunks ever added" rather than "chunks resident". Terrain streaming hides
	// that -- its churn is slow -- but water re-meshes at 10 Hz and its bricks
	// appear and vanish continuously, so the table crosses the proxy's MaxChunks
	// headroom within a minute or two. Crossing it is not a slow leak but a
	// cliff: PushUpdatesToProxy falls back to MarkRenderStateDirty, which throws
	// the proxy away and re-uploads the WHOLE pool buffer -- the exact cost the
	// incremental path exists to avoid, now on a repeating timer.
	//
	// Recycling is safe because RemoveChunk first repoints every one of the
	// chunk's quads at kHiddenChunkId, so no quad in the pool still references
	// the id being handed back.
	TArray<uint32> FreeChunkIds;

	FVoxelGpuGeometryPool Pool;
	TArray<FVoxelGpuPoolAllocation> Allocations;  // indexed by handle
	// Parallel to Allocations: which table entry each handle owns. Kept
	// explicitly rather than re-derived from QuadChunkIds[Offset] so that
	// freeing the entry does not depend on the allocation still being non-empty.
	TArray<uint32> AllocationChunkIds;
	int32 NumLiveChunks = 0;

	// See GetAllocFailureCount. Cumulative for the pool's lifetime; never reset
	// by ClearChunks, because the question these answer is "did this RUN ever
	// drop geometry", not "is it dropping geometry right now".
	int64 AllocFailureCount = 0;
	int64 AllocFailureQuads = 0;

	// Ranges written since the last upload, in quads. Streaming touches a few
	// chunks per frame out of thousands, so uploading the whole pool for each
	// change would be absurd -- at cascade scale that is 75 MB per edit.
	//
	// A LIST, NOT ONE SPAN (Wave D / D1, path 2). This used to be a single
	// {First, Last} merged with Min/Max, which meant two chunks written at
	// distant pool offsets marked EVERYTHING BETWEEN THEM dirty and re-uploaded
	// it from the CPU shadow. That is a large amount of pointless upload on the
	// CPU path -- and once the GPU writes quads into the pool directly, it is
	// silent corruption: any GPU-written range caught in the gap between two
	// CPU-written chunks gets overwritten with stale shadow content, on every
	// add or remove.
	//
	// The span was always an OVER-APPROXIMATION of what the CPU actually wrote:
	// every dirty region comes from a write to one chunk's range, and the
	// Min/Max merge threw that precision away. Keeping the intervals is not
	// extra bookkeeping, it is declining to discard information already in hand.
	//
	// The property that makes this the whole fix for path 2: an interval only
	// ever covers quads the CPU WROTE, so a GPU-written range is untouchable by
	// construction -- no GPU-range tracking, nothing to keep in sync, and no way
	// for the two to drift apart.
	struct FDirtyRange { uint32 First = 0; uint32 Last = 0; bool bValid = false; };
	TArray<FDirtyRange> DirtyQuadRanges;
	bool bChunkTableDirty = false;

	// Set whenever the set of live ALLOCATIONS changes -- which is not the same
	// event as the chunk table changing, and conflating the two is what made the
	// proxy's cull test one chunk's bounds against another chunk's quads.
	// UpdateChunk's realloc branch moves a chunk to a new pool offset while
	// deliberately keeping its table entry (so an actively-dug chunk does not burn
	// a fresh entry per edit), so it leaves the table clean and the runs wrong: the
	// run still names the offset the chunk used to occupy, which the allocator is
	// free to hand to somebody else. A run's ChunkId then describes a different
	// chunk from the quads at its FirstQuad, and a perfectly correct frustum test
	// on a perfectly correct bounding box selects the wrong geometry.
	bool bRunsDirty = false;

	// Rebuilt from Allocations whenever the chunk table changes, and handed to
	// the proxy with it. Live chunks only -- a freed handle contributes nothing.
	TArray<FChunkRun> BuildChunkRuns() const;

	void MarkQuadsDirty(uint32 First, uint32 Count);
	void PushUpdatesToProxy();

	// RemoveChunk's body. bRecycleChunkId is false for the one caller that means
	// to keep the table entry alive across the removal -- UpdateChunk's realloc
	// branch, which re-uses the same entry for the chunk's new range so that an
	// actively-dug chunk does not consume a fresh table slot on every edit.
	void RemoveChunkInternal(int32 Handle, bool bRecycleChunkId);

	// Set while a proxy is live so updates can go straight to it instead of
	// rebuilding the render state. Render-thread lifetime is owned by the
	// renderer; this is only ever dereferenced inside a render command.
	class FVoxelGpuPoolSceneProxy* LiveProxy = nullptr;

	FBox LocalBounds = FBox(ForceInit);

	FString PoolName = TEXT("VoxelGpuPool");
	int32 ChunkEdgeVoxels = 32;
	int32 ChunkTableCapacity = 1024;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> ChunkMaterial = nullptr;
};
