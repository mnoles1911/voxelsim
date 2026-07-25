#include "VoxelQuadVertexFactory.h"

#include "MeshMaterialShader.h"
#include "MeshBatch.h"
#include "MeshDrawShaderBindings.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "CommonRenderResources.h"

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

	// Set to 1 to replace all geometry with one 20 m quad -- see the
	// VOXEL_VF_DEBUG_QUAD block in VoxelQuadVertexFactory.ush. Off by default.
	OutEnvironment.SetDefine(TEXT("VOXEL_VF_DEBUG_QUAD"), 1);
}

void FVoxelQuadVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	// ONE ZERO-STRIDE DUMMY STREAM. Nothing reads it.
	//
	// An empty vertex declaration was tried first, and it was wrong. It compiles,
	// the proxy is created, GetDynamicMeshElements is called, the draw is
	// issued -- and nothing rasterises, silently. Proven by bisect: a hardcoded
	// 20 m quad that ignored all buffer data was equally invisible, so the fault
	// was never in the geometry.
	//
	// GEmptyVertexDeclaration does work for Nanite's raster passes, but those
	// bypass the mesh-pass system entirely and set up their own PSO. A draw that
	// goes through FMeshDrawCommand needs a stream to hang its vertex parameters
	// on. So do what FLidarPointCloudVertexFactory does: bind one zero-stride
	// element pointing at the engine's null colour buffer purely to give the
	// declaration something to describe. Stride 0 means every vertex reads the
	// same 4 bytes; no per-vertex memory is consumed and the shader ignores it.
	//
	// Geometry still comes entirely from SV_VertexID. This is a formality the
	// draw path demands, not a real vertex stream.
	FVertexDeclarationElementList Elements;
	Elements.Add(AccessStreamComponent(
		FVertexStreamComponent(&GNullColorVertexBuffer, 0, 0, VET_Color), 0));
	InitDeclaration(Elements);

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
