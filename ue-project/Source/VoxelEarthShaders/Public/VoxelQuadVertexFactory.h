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
	SHADER_PARAMETER(FVector3f, ChunkOriginUU)
	SHADER_PARAMETER(float, LevelScale)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

// Per-chunk framing, bound PER DRAW rather than per factory.
//
// This is what lets one vertex factory -- and therefore one primitive -- serve
// many chunks out of a shared pool. Each chunk's quads are contiguous in the
// pool, so each draw covers one range and needs only its own origin and mip
// scale. That is the whole point of ADR-0006: streaming a chunk in or out
// writes into the pool and changes a draw range, and never touches FScene.
struct FVoxelChunkDrawData
{
	FVector3f ChunkOriginUU = FVector3f::ZeroVector;
	float LevelScale = 1.0f;
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

	FRHIUniformBuffer* GetUniformBuffer() const { return UniformBuffer.GetReference(); }

private:
	FShaderResourceViewRHIRef QuadBufferSRV;
	FVector3f ChunkOriginUU = FVector3f::ZeroVector;
	float LevelScale = 1.0f;
	TUniformBufferRef<FVoxelQuadVertexFactoryParameters> UniformBuffer;
};
