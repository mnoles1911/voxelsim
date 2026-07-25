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
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelQuadVertexFactoryParameters, )
	SHADER_PARAMETER_SRV(StructuredBuffer<uint2>, VoxelVF_QuadBuffer)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

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

	// The pool this factory reads. Must be set before InitRHI; changing it
	// afterwards needs a re-init so the uniform buffer picks up the new SRV.
	void SetQuadBufferSRV(FShaderResourceViewRHIRef InSRV) { QuadBufferSRV = MoveTemp(InSRV); }

	FRHIUniformBuffer* GetUniformBuffer() const { return UniformBuffer.GetReference(); }

private:
	FShaderResourceViewRHIRef QuadBufferSRV;
	TUniformBufferRef<FVoxelQuadVertexFactoryParameters> UniformBuffer;
};
