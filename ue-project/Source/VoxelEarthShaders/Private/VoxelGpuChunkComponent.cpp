#include "VoxelGpuChunkComponent.h"

#include "VoxelQuadVertexFactory.h"
#include "PrimitiveSceneProxy.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MeshBatch.h"
#include "SceneManagement.h"
#include "RHIResourceUtils.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "MaterialShared.h"
#include "Materials/MaterialRenderProxy.h"

namespace
{
	// Chunks are 32 voxels on a side at 10 cm, scaled by the mip level.
	// VoxelCoords::ChunkEdgeVoxels * VoxelCoords::VoxelSizeUU = 32 * 10.
	constexpr double kChunkEdgeUU = 320.0;
}

// Draws one chunk's quads with no vertex buffer and no index buffer.
class FVoxelGpuChunkSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		// One static per proxy CLASS, not per instance. Getting this wrong
		// silently corrupts the renderer's state-bucket/PSO caching.
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	FVoxelGpuChunkSceneProxy(UVoxelGpuChunkComponent* Component, const TArray<uint64>& InQuads, int32 InLevel)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel())
		, Quads(InQuads)
		, NumQuads(InQuads.Num())
		, LevelScale(float(1 << InLevel))
		, MaterialProxy(nullptr)
	{
		UMaterialInterface* Material = Component->GetChunkMaterialOrDefault();
		MaterialProxy = Material->GetRenderProxy();

		// Register the material for FMeshBatch::Validate's VerifyUsedMaterial
		// check. Without this the batch is dropped before any mesh pass sees it.
		#if !(UE_BUILD_SHIPPING)
		{
			TArray<UMaterialInterface*> UsedForVerification;
			UsedForVerification.Add(Material);
			SetUsedMaterialForVerification(UsedForVerification);
		}
		#endif
		MaterialRelevance = Material->GetRelevance_Concurrent(GetScene().GetFeatureLevel());
	}

	virtual ~FVoxelGpuChunkSceneProxy()
	{
		VertexFactory.ReleaseResource();
	}

	void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override
	{
		if (NumQuads == 0)
		{
			return;
		}

		// The quad buffer is the only GPU resource this chunk owns. There is no
		// vertex or index buffer at all -- the vertex shader rebuilds every
		// corner from SV_VertexID.
		const uint32 Stride = sizeof(uint64);
		const uint32 SizeBytes = Stride * uint32(NumQuads);

		QuadBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
			RHICmdList,
			TEXT("VoxelGpuChunk.Quads"),
			EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer,
			MakeConstArrayView(Quads));

		QuadBufferSRV = RHICmdList.CreateShaderResourceView(
			QuadBuffer, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(QuadBuffer));

		// The factory bakes the SRV and framing into a uniform buffer during
		// InitResource, so both must be set first.
		VertexFactory.SetQuadBufferSRV(QuadBufferSRV);
		VertexFactory.SetChunkFraming(FVector3f::ZeroVector, LevelScale);
		VertexFactory.InitResource(RHICmdList);

		// Quads no longer needed on the CPU once they are on the GPU.
		Quads.Empty();
	}

	FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();

		// Dynamic, not static -- see docs/gpu-g2-draw-path.md. The amount of
		// geometry drawn changes as chunks stream, and a cached static batch
		// cannot have its NumPrimitives changed without re-caching the
		// primitive's draw commands.
		Result.bDynamicRelevance = true;

		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		return Result;
	}

	void GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	                            const FSceneViewFamily& ViewFamily,
	                            uint32 VisibilityMap,
	                            FMeshElementCollector& Collector) const override
	{
		if (NumQuads == 0 || !QuadBufferSRV.IsValid() || MaterialProxy == nullptr)
		{
			return;
		}

		// One-shot proof of life. "Nothing renders" has several very different
		// causes -- culled by bounds, proxy never created, draw issued but
		// producing no pixels -- and they are indistinguishable from a
		// screenshot. This says which side of the line we are on.
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			bLoggedOnce = true;
			UE_LOG(LogTemp, Warning,
			       TEXT("VoxelGpuChunk: GetDynamicMeshElements called — %d views, %d quads, SRV %s"),
			       Views.Num(), NumQuads, QuadBufferSRV.IsValid() ? TEXT("valid") : TEXT("NULL"));

			// THE DECISIVE CHECK. The batch dies between AddMesh and shader
			// binding, and by far the most likely reason is that the material's
			// shader map has no entry for this vertex factory type -- which is
			// precisely the case the pass processor handles by silently
			// dropping the batch. Ask the shader map directly instead of
			// inferring it from a blank screen.
			const ERHIFeatureLevel::Type FeatureLevel = GetScene().GetFeatureLevel();
			const FMaterialRenderProxy* Fallback = MaterialProxy;
			const FMaterial& Mat = MaterialProxy->GetMaterialWithFallback(FeatureLevel, Fallback);
			const FMaterialShaderMap* ShaderMap = Mat.GetRenderingThreadShaderMap();
			const FMeshMaterialShaderMap* MeshMap =
				ShaderMap ? ShaderMap->GetMeshShaderMap(VertexFactory.GetType()) : nullptr;

			UE_LOG(LogTemp, Warning,
			       TEXT("VoxelGpuChunk: material '%s' fellBack=%d shaderMap=%s meshShaderMapForVF=%s"),
			       *Mat.GetFriendlyName(),
			       (Fallback != MaterialProxy) ? 1 : 0,
			       ShaderMap ? TEXT("present") : TEXT("NULL"),
			       MeshMap ? TEXT("PRESENT") : TEXT("MISSING <-- batch will be dropped"));
		}

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if ((VisibilityMap & (1 << ViewIndex)) == 0)
			{
				continue;
			}

			FMeshBatch& Mesh = Collector.AllocateMesh();
			Mesh.VertexFactory = &VertexFactory;
			Mesh.MaterialRenderProxy = MaterialProxy;
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.bCanApplyViewModeOverrides = false;
			Mesh.CastShadow = IsShadowCast(Views[ViewIndex]);
			Mesh.bWireframe = false;

			FMeshBatchElement& Element = Mesh.Elements[0];

			// NO INDEX BUFFER. A null index buffer routes the submit path to
			// DrawPrimitive rather than DrawIndexedPrimitive, and SV_VertexID
			// then runs [0, NumPrimitives*3) -- which the vertex factory
			// divides by 6 to recover the quad and its corner.
			Element.IndexBuffer = nullptr;
			Element.FirstIndex = 0;
			Element.NumPrimitives = uint32(NumQuads) * 2;  // two triangles per quad
			Element.MinVertexIndex = 0;
			Element.MaxVertexIndex = uint32(NumQuads) * 6 - 1;
			Element.NumInstances = 1;
			Element.PrimitiveUniformBuffer = GetUniformBuffer();
			// UserData stays null. It used to point at a proxy member of the
			// now-deleted FVoxelChunkDrawData, which nothing ever read. The
			// vertex factory DOES read UserData now (as FVoxelQuadRangeUserData),
			// so leaving a differently-typed pointer there would have been a
			// type confusion rather than merely dead. Null means "start at pool
			// quad 0", which is what this single-chunk draw wants.

			Collector.AddMesh(ViewIndex, Mesh);
		}
	}

	uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

private:
	mutable FVoxelQuadVertexFactory VertexFactory;

	TArray<uint64> Quads;   // emptied once uploaded
	FBufferRHIRef QuadBuffer;
	FShaderResourceViewRHIRef QuadBufferSRV;

	int32 NumQuads = 0;
	float LevelScale = 1.0f;

	FMaterialRenderProxy* MaterialProxy;
	FMaterialRelevance MaterialRelevance;
};

UVoxelGpuChunkComponent::UVoxelGpuChunkComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	CastShadow = true;
	bUseAsOccluder = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void UVoxelGpuChunkComponent::SetQuads(const TArray<uint64>& InQuads)
{
	Quads = InQuads;
	MarkRenderStateDirty();
	UpdateBounds();
}

void UVoxelGpuChunkComponent::SetChunkLevel(int32 InLevel)
{
	ChunkLevel = InLevel;
	MarkRenderStateDirty();
	UpdateBounds();
}

void UVoxelGpuChunkComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials,
                                              bool bGetDebugMaterials) const
{
	OutMaterials.Add(GetChunkMaterialOrDefault());
}

void UVoxelGpuChunkComponent::SetChunkMaterial(UMaterialInterface* InMaterial)
{
	ChunkMaterial = InMaterial;
	MarkRenderStateDirty();
}

UMaterialInterface* UVoxelGpuChunkComponent::GetChunkMaterialOrDefault() const
{
	return ChunkMaterial ? ChunkMaterial.Get() : UMaterial::GetDefaultMaterial(MD_Surface);
}

FPrimitiveSceneProxy* UVoxelGpuChunkComponent::CreateSceneProxy()
{
	if (Quads.IsEmpty())
	{
		return nullptr;
	}
	return new FVoxelGpuChunkSceneProxy(this, Quads, ChunkLevel);
}

FBoxSphereBounds UVoxelGpuChunkComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// Derived from the quads rather than assumed from the chunk size.
	//
	// The assumption was wrong and would have caused silent culling: this
	// component is fed a 64-voxel REGION (640 UU), not a 32-voxel chunk
	// (320 UU), so a chunk-sized box covered half the geometry. Scanning a few
	// thousand packed quads costs nothing next to being invisible for a reason
	// no screenshot can show.
	FBox LocalBox(ForceInit);
	const float Scale = float(1 << ChunkLevel) * 10.0f;  // VoxelSizeUU

	for (const uint64 Packed : Quads)
	{
		const uint32 W0 = uint32(Packed & 0xffffffffull);
		const uint32 W1 = uint32(Packed >> 32);

		const uint32 Axis  =  W0        & 0xfu;
		const uint32 Slice = (W0 >>  8) & 0xffu;
		const uint32 U0    = (W0 >> 16) & 0xffu;
		const uint32 V0    = (W0 >> 24) & 0xffu;
		const uint32 QW    =  W1        & 0xffu;
		const uint32 QH    = (W1 >>  8) & 0xffu;

		const uint32 U = (Axis + 1u) % 3u;
		const uint32 V = (Axis + 2u) % 3u;

		FVector Lo(0.0), Hi(0.0);
		Lo[Axis] = double(Slice);       Hi[Axis] = double(Slice) + 1.0;
		Lo[U]    = double(U0);          Hi[U]    = double(U0 + QW);
		Lo[V]    = double(V0);          Hi[V]    = double(V0 + QH);

		LocalBox += Lo * Scale;
		LocalBox += Hi * Scale;
	}

	if (!LocalBox.IsValid)
	{
		const double EdgeUU = kChunkEdgeUU * double(1 << ChunkLevel);
		LocalBox = FBox(FVector::ZeroVector, FVector(EdgeUU, EdgeUU, EdgeUU));
	}

	return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
}
