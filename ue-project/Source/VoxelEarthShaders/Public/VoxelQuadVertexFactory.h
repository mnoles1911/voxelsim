// Vertex factory for GPU-resident voxel quads (ADR-0006, G2).
//
// Draws geometry that has no vertex buffer. The GPU mesher writes packed quads
// into a persistent pool; this factory hands that pool to the vertex shader as
// an SRV, and the shader rebuilds every corner from SV_VertexID. See
// Shaders/VoxelQuadVertexFactory.ush.
//
// Deliberately minimal: no static lighting (procedural geometry is never
// lightmapped), no GPUScene primitive-id stream, no ray tracing. Each of those
// costs another required .ush hook, and none of them is needed to prove G2.

#pragma once

#include "CoreMinimal.h"
#include "VertexFactory.h"
#include "RenderResource.h"
#include "ShaderParameters.h"

// The quad pool SRV, handed to the shader in one uniform buffer. A stable
// FRHIUniformBuffer* stays hashable, which keeps the door open to draw-command
// caching later; a loose shader parameter rebound every draw would not.
// NOTE ON NAMING: this struct is registered under the shader-side name
// "VoxelVF", so in HLSL these members are reached as VoxelVF.QuadBuffer,
// NOT as loose globals. Declaring a loose `StructuredBuffer<uint2> QuadBuffer;`
// in the .ush instead would compile perfectly and then read zeros forever,
// because it would be a different, unbound symbol. Black terrain, no error.
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelQuadVertexFactoryParameters, )
	SHADER_PARAMETER_SRV(StructuredBuffer<uint2>, QuadBuffer)
	// Single-chunk framing (the G2 component). Ignored when PoolMode is 1.
	SHADER_PARAMETER(FVector3f, ChunkOriginUU)
	SHADER_PARAMETER(float, LevelScale)

	// POOL MODE: many chunks, one factory, one draw.
	//
	// ChunkOrigins holds one entry per resident chunk (xyz = origin in unreal
	// units, w = mip scale). QuadChunkIds names the owning chunk for every quad
	// in the pool. The vertex shader then needs NO per-draw state at all --
	// which is what lets a single draw span every chunk, and is why this uses
	// SRVs in the uniform buffer rather than loose per-element parameters
	// (which do not bind; see docs/gpu-g2-draw-path.md).
	SHADER_PARAMETER_SRV(StructuredBuffer<float4>, ChunkOrigins)
	SHADER_PARAMETER_SRV(StructuredBuffer<uint>, QuadChunkIds)
	// Per-chunk shading inputs the quad packing has no room for.
	//
	//   x = temperature, y = precipitation, both already remapped to 0..1
	//       across this world's p1..p99 window. The material feeds them to the
	//       biome LUT. Sampled once per chunk on the CPU -- a chunk is ~1/100th
	//       the area of one 30 m climate pixel and the ramp across it is smooth,
	//       so the error is a gentle gradient rather than banding.
	//   z = the terrain surface height at this chunk's centre, expressed
	//       RELATIVE TO THIS CHUNK'S OWN ORIGIN in unreal units. That framing is
	//       deliberate: it makes the value small and rebase-independent, so no
	//       float32 precision is spent on the 84 km world offset (see
	//       docs/gpu-pool-rendering-notes.md invariant 4). The vertex factory
	//       compares a corner's chunk-local Z against it to reproduce
	//       BuildChunkVertexData's surface-proximity gate exactly.
	//   w = reserved (per-chunk debug tint, docs/gpu-g4-parity-plan.md item 2).
	//
	// Widened from float2 to float4 for z; at a table sized in thousands of
	// entries the extra 8 bytes per chunk is noise against the quad pool.
	SHADER_PARAMETER_SRV(StructuredBuffer<float4>, ChunkParams)
	SHADER_PARAMETER(uint32, PoolMode)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

// WHERE THIS DRAW STARTS IN THE POOL, in quads. Bound PER BATCH ELEMENT.
//
// SV_VertexID DOES NOT INCLUDE THE DRAW'S BASE VERTEX on D3D12. The engine says
// so itself -- RHISupportsAbsoluteVertexID (DataDrivenShaderPlatformInfo.h)
// returns true only for Vulkan, and FLocalVertexFactory compensates by
// threading the base through a uniform buffer and adding it in the shader
// (VF_VertexOffset). Measured here, not assumed: with the frustum test bypassed
// and the pool tiled into N exact contiguous ranges
// (voxel.Stream.GPUCullDebugSplit), only the FIRST 1/N of the pool reached the
// screen at N = 2, 8 and 64, while the frame cost stayed flat (12.26 -> 11.86 ms
// at N=8) and then ROSE at N=64 (14.07 ms) -- i.e. every range still processed
// its full quad count, starting at pool quad 0, and paid for 63 extra draws on
// top. See docs/gpu-waves-plan.md "Wave A".
//
// So every range draws from vertex 0 and names its start explicitly. The
// alternative -- expressing the start as FMeshBatchElement::FirstIndex and
// trusting SV_VertexID to carry it -- is what produced the picture above.
//
// A UNIFORM BUFFER, not a loose FShaderParameter. Loose vertex-factory
// parameters do not bind in this project (measured; docs/gpu-g2-draw-path.md),
// and ShaderBindings.Add() on an unbound parameter is a SILENT no-op -- so the
// loose version would have compiled, run, logged nothing, and drawn the wrong
// geometry. This is the same shape FVoxelGIVolumeParameters already uses in the
// same hook.
//
// NAMING: registered shader-side as "VoxelRange", so HLSL reaches this as
// VoxelRange.BaseQuad. A loose `uint BaseQuad;` in the .ush would be a
// different, unbound symbol that reads zero forever -- which is exactly the
// pre-fix behaviour, silently.
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelQuadRangeParameters, )
	SHADER_PARAMETER(uint32, BaseQuad)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

// What FMeshBatchElement::UserData points at on the pooled path.
//
// MUST be allocated with FMeshElementCollector::AllocateOneFrameResource<T>().
// A stack local, or a member of the proxy, does not survive to draw submission:
// GetDynamicMeshElements only GATHERS batches, and the bindings are read later
// when the mesh draw commands are built. (Do not copy the older
// VoxelGpuChunkComponent pattern of pointing UserData at a proxy member -- that
// was dead code nothing read, and it is gone.)
struct FVoxelQuadRangeUserData
{
	TUniformBufferRef<FVoxelQuadRangeParameters> RangeUniformBuffer;
};

class VOXELEARTHSHADERS_API FVoxelQuadVertexFactory : public FVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FVoxelQuadVertexFactory);

public:
	explicit FVoxelQuadVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FVertexFactory(InFeatureLevel) {}

	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment);

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;

	// What this factory draws. All three must be set before InitRHI, because
	// the uniform buffer is built there; changing any of them afterwards needs
	// a re-init so the buffer picks up the new values.
	void SetQuadBufferSRV(FShaderResourceViewRHIRef InSRV) { QuadBufferSRV = MoveTemp(InSRV); }
	void SetChunkFraming(const FVector3f& InOriginUU, float InLevelScale)
	{
		ChunkOriginUU = InOriginUU;
		LevelScale = InLevelScale;
	}

	// Switches the factory to pool mode: per-quad chunk ids index a per-chunk
	// origin table, so one factory serves every chunk in the pool.
	void SetPoolBuffers(FShaderResourceViewRHIRef InOrigins, FShaderResourceViewRHIRef InChunkIds,
	                    FShaderResourceViewRHIRef InParams)
	{
		ChunkOriginsSRV = MoveTemp(InOrigins);
		QuadChunkIdsSRV = MoveTemp(InChunkIds);
		ChunkParamsSRV = MoveTemp(InParams);
		bPoolMode = true;
	}

	FRHIUniformBuffer* GetUniformBuffer() const { return UniformBuffer.GetReference(); }

	// BaseQuad = 0, built once. Bound for any element that carries no range of
	// its own -- the single full-pool draw and the single-chunk component -- so
	// those paths compute QuadIndex = 0 + VertexId/6, which is byte for byte
	// what they computed before this parameter existed.
	//
	// It has to be a real buffer rather than nullptr: ShaderBindings.Add()
	// checkf()s a non-null value once the parameter IS bound, and it is bound in
	// every permutation because the shader always reads it.
	FRHIUniformBuffer* GetZeroRangeUniformBuffer() const { return ZeroRangeUniformBuffer.GetReference(); }

private:
	FShaderResourceViewRHIRef QuadBufferSRV;
	FVector3f ChunkOriginUU = FVector3f::ZeroVector;
	float LevelScale = 1.0f;
	FShaderResourceViewRHIRef ChunkOriginsSRV;
	FShaderResourceViewRHIRef QuadChunkIdsSRV;
	FShaderResourceViewRHIRef ChunkParamsSRV;
	bool bPoolMode = false;
	TUniformBufferRef<FVoxelQuadVertexFactoryParameters> UniformBuffer;
	TUniformBufferRef<FVoxelQuadRangeParameters> ZeroRangeUniformBuffer;
};
