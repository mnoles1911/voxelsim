#include "VoxelQuadVertexFactory.h"

#include "MeshMaterialShader.h"
#include "MeshBatch.h"
#include "MeshDrawShaderBindings.h"
#include "DataDrivenShaderPlatformInfo.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelQuadVertexFactoryParameters, "VoxelVF");

bool FVoxelQuadVertexFactory::ShouldCompilePermutation(
	const FVertexFactoryShaderPermutationParameters& Parameters)
{
	// WHY THIS GATE MATTERS: without one, every surface material in the project
	// compiles a permutation against this factory. That is what makes
	// FLocalVertexFactory so expensive to iterate on, and it is easy to inherit
	// by accident.
	//
	// The engine's usual gate is a per-material usage flag (bIsUsedWithWater,
	// bIsUsedWithLidarPointCloud, ...), but those are a fixed engine enum and
	// adding one means editing engine source. Only a handful of materials will
	// ever be assigned to voxel terrain, so gating on the surface domain keeps
	// the blow-up bounded without an engine modification. If material compile
	// times ever become the complaint, adding a real bUsedWithVoxelTerrain flag
	// is the clean fix -- see docs/gpu-g2-draw-path.md.
	//
	// bIsSpecialEngineMaterial must always be allowed through, or the default
	// material fallback cannot compile against this factory and anything using
	// it renders as the "missing material" case.
	const bool bIsSurface = Parameters.MaterialParameters.MaterialDomain == MD_Surface;
	return (bIsSurface || Parameters.MaterialParameters.bIsSpecialEngineMaterial)
	    && IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FVoxelQuadVertexFactory::ModifyCompilationEnvironment(
	const FVertexFactoryShaderPermutationParameters& Parameters,
	FShaderCompilerEnvironment& OutEnvironment)
{
	FVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
}

void FVoxelQuadVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	// EMPTY VERTEX DECLARATION, ON PURPOSE.
	//
	// This factory binds no vertex streams at all -- SV_VertexID is a system
	// value, not an attribute, so the vertex shader needs nothing bound to
	// reconstruct geometry. InitDeclaration forwards an empty element list to
	// GetOrCreateVertexDeclaration with no minimum-element requirement, which
	// yields the same kind of object as the engine's own GEmptyVertexDeclaration
	// (used by Nanite's raster passes for exactly this reason).
	//
	// If a platform ever rejects this, the fallback is FLidarPointCloudVertexFactory's
	// trick: bind one zero-stride dummy stream purely so a declaration exists.
	InitDeclaration(FVertexDeclarationElementList());

	if (QuadBufferSRV.IsValid())
	{
		FVoxelQuadVertexFactoryParameters Parameters;
		Parameters.QuadBuffer = QuadBufferSRV;
		Parameters.ChunkOriginUU = ChunkOriginUU;
		Parameters.LevelScale = LevelScale;
		UniformBuffer = TUniformBufferRef<FVoxelQuadVertexFactoryParameters>::CreateUniformBufferImmediate(
			Parameters, UniformBuffer_MultiFrame);
	}
}

void FVoxelQuadVertexFactory::ReleaseRHI()
{
	UniformBuffer.SafeRelease();
	QuadBufferSRV.SafeRelease();
	FVertexFactory::ReleaseRHI();
}

// Binds the factory's uniform buffer for each draw.
class FVoxelQuadVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FVoxelQuadVertexFactoryShaderParameters, NonVirtual);

public:
	void Bind(const FShaderParameterMap& ParameterMap) {}

	void GetElementShaderBindings(
		const FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const
	{
		// One pool per factory instance in G2, so the buffer can be read
		// straight off the factory rather than threaded through the batch
		// element. G3 will need per-chunk ranges and will move to UserData.
		const FVoxelQuadVertexFactory* Factory = static_cast<const FVoxelQuadVertexFactory*>(VertexFactory);
		if (FRHIUniformBuffer* Uniforms = Factory->GetUniformBuffer())
		{
			ShaderBindings.Add(
				Shader->GetUniformBufferParameter<FVoxelQuadVertexFactoryParameters>(), Uniforms);
		}
	}
};

IMPLEMENT_TYPE_LAYOUT(FVoxelQuadVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FVoxelQuadVertexFactory, SF_Vertex,
                                        FVoxelQuadVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FVoxelQuadVertexFactory,
	"/VoxelEarth/VoxelQuadVertexFactory.ush",
	  EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
);
