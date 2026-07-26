#include "VoxelQuadVertexFactory.h"

#include "VoxelGIVolume.h"
#include "MeshMaterialShader.h"
#include "MeshBatch.h"
#include "MeshDrawShaderBindings.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "CommonRenderResources.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelQuadVertexFactoryParameters, "VoxelVF");
IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelQuadRangeParameters, "VoxelRange");

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
	OutEnvironment.SetDefine(TEXT("VOXEL_VF_DEBUG_QUAD"), 0);
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

	// The BaseQuad = 0 fallback. Built unconditionally -- the single-chunk
	// component has no QuadBufferSRV set at this point in some paths, and every
	// permutation of the shader reads VoxelRange.BaseQuad regardless.
	{
		FVoxelQuadRangeParameters ZeroRange;
		ZeroRange.BaseQuad = 0u;
		ZeroRangeUniformBuffer = TUniformBufferRef<FVoxelQuadRangeParameters>::CreateUniformBufferImmediate(
			ZeroRange, UniformBuffer_MultiFrame);
	}

	if (QuadBufferSRV.IsValid())
	{
		FVoxelQuadVertexFactoryParameters Parameters;
		Parameters.QuadBuffer = QuadBufferSRV;
		Parameters.ChunkOriginUU = ChunkOriginUU;
		Parameters.LevelScale = LevelScale;
		Parameters.PoolMode = bPoolMode ? 1u : 0u;
		// Both SRVs must be non-null even in single-chunk mode: an unbound SRV
		// in a uniform buffer is a validation failure, not a tolerated no-op.
		Parameters.ChunkOrigins = ChunkOriginsSRV.IsValid() ? ChunkOriginsSRV : QuadBufferSRV;
		Parameters.QuadChunkIds = QuadChunkIdsSRV.IsValid() ? QuadChunkIdsSRV : QuadBufferSRV;
		Parameters.ChunkParams = ChunkParamsSRV.IsValid() ? ChunkParamsSRV : QuadBufferSRV;
		UniformBuffer = TUniformBufferRef<FVoxelQuadVertexFactoryParameters>::CreateUniformBufferImmediate(
			Parameters, UniformBuffer_MultiFrame);
	}
}

void FVoxelQuadVertexFactory::ReleaseRHI()
{
	UniformBuffer.SafeRelease();
	ZeroRangeUniformBuffer.SafeRelease();
	QuadBufferSRV.SafeRelease();
	FVertexFactory::ReleaseRHI();
}

// Binds the factory's uniform buffer for each draw.
class FVoxelQuadVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FVoxelQuadVertexFactoryShaderParameters, NonVirtual);

	// NO LOOSE FShaderParameters HERE, deliberately. Two used to sit in this
	// slot -- VoxelChunkOriginUU and VoxelChunkLevelScale -- and both were dead:
	// neither name exists in the .ush any more, both moved into the VoxelVF
	// uniform buffer because loose parameters measurably do not bind here, and
	// IsBound() reported 0 for both on every launch. Anything per-element this
	// factory needs goes through a uniform buffer; see FVoxelQuadRangeParameters.

public:
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		// Required by IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE and intentionally
		// empty: everything this factory binds is a uniform buffer, which is
		// resolved by type in GetElementShaderBindings rather than by name here.
	}

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
		// Unconditional, before any branch: this answers "is it called at all",
		// separately from "is UserData set".
		static bool bLoggedEntry = false;
		if (!bLoggedEntry)
		{
			bLoggedEntry = true;
			UE_LOG(LogTemp, Warning, TEXT("VoxelVF: GetElementShaderBindings ENTERED"));
		}

		// The pool itself is per FACTORY -- one factory serves every chunk -- so
		// it is read straight off the factory.
		const FVoxelQuadVertexFactory* Factory = static_cast<const FVoxelQuadVertexFactory*>(VertexFactory);
		if (FRHIUniformBuffer* Uniforms = Factory->GetUniformBuffer())
		{
			ShaderBindings.Add(
				Shader->GetUniformBufferParameter<FVoxelQuadVertexFactoryParameters>(), Uniforms);
		}

		// WHERE THIS ELEMENT STARTS IN THE POOL. This is the per-element state
		// the comment that used to sit here promised G3 would need.
		//
		// UserData is the only channel that is per BATCH ELEMENT rather than per
		// batch, and a frustum-culled pool submits one element per surviving pool
		// range. An element that carries none -- the single full-pool draw, and
		// the single-chunk component -- falls back to the factory's BaseQuad = 0
		// buffer, which makes those paths identical to what they were before this
		// existed rather than merely equivalent.
		//
		// Never pass nullptr: Add() checkf()s a non-null value for a bound
		// uniform-buffer parameter, and this one is bound in every permutation.
		FRHIUniformBuffer* RangeUniforms = Factory->GetZeroRangeUniformBuffer();
		if (const FVoxelQuadRangeUserData* Range =
		        static_cast<const FVoxelQuadRangeUserData*>(BatchElement.UserData))
		{
			if (FRHIUniformBuffer* FromElement = Range->RangeUniformBuffer.GetReference())
			{
				RangeUniforms = FromElement;
			}
		}
		if (RangeUniforms != nullptr)
		{
			ShaderBindings.Add(
				Shader->GetUniformBufferParameter<FVoxelQuadRangeParameters>(), RangeUniforms);
		}

		// The GI volume is a SECOND uniform buffer rather than more members on
		// the factory's own, which is built once in InitRHI and documented as
		// needing a re-init to change. This one changes whenever the volume
		// re-centres. GetUniformBuffer() is never null -- when GI is off it
		// points at GBlackVolumeTexture with Enabled=0, because an unbound
		// member is a validation failure rather than a tolerated no-op.
		if (FRHIUniformBuffer* GIUniforms = GVoxelGIVolume.GetUniformBuffer())
		{
			ShaderBindings.Add(
				Shader->GetUniformBufferParameter<FVoxelGIVolumeParameters>(), GIUniforms);
		}
	}
};

IMPLEMENT_TYPE_LAYOUT(FVoxelQuadVertexFactoryShaderParameters);

// Register BOTH frequencies. CreateShaderParameters dispatches through
// TVertexFactoryParameterTraits<frequency, type> and returns null for any
// (type, frequency) pair that was never specialised -- and a null parameters
// object makes FMeshMaterialShader::GetElementShaderBindings skip the vertex
// factory entirely, with no else, no ensure and no log.
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FVoxelQuadVertexFactory, SF_Vertex,
                                        FVoxelQuadVertexFactoryShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FVoxelQuadVertexFactory, SF_Pixel,
                                        FVoxelQuadVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FVoxelQuadVertexFactory,
	"/VoxelEarth/VoxelQuadVertexFactory.ush",
	  EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
);
