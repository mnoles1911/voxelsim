#include "VoxelWaterChunkComponent.h"

#include "VoxelCoords.h"

#include "DynamicMeshBuilder.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveDrawingUtils.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "PrimitiveViewRelevance.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "StaticMeshResources.h"

// FWaterChunkSceneProxy -----------------------------------------------------
//
// Doctrine (docs/voxel-earth-implementation-plan.md SS3.3 Band 1, extended to
// water by the W2 task spec): hand-rolled FPrimitiveSceneProxy, NOT
// ProceduralMeshComponent. Structurally a lean copy of FVoxelChunkSceneProxy
// (VoxelChunkComponent.cpp) with the ring mip-level scale and cross-fade
// material params dropped -- v0 active water never mip-levels (see this
// component's header comment) -- and a translucent water material in place
// of terrain's opaque/masked one.
class FWaterChunkSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	explicit FWaterChunkSceneProxy(UWaterChunkComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel(), "FWaterChunkSceneProxy")
	{
		static const FVector3f AxisDir[3] = {FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1)};

		constexpr float VoxelSizeUU = float(VoxelCoords::VoxelSizeUU);

		const int32 NumQuads = Component->ChunkQuads.Num();
		TArray<FDynamicMeshVertex> Vertices;
		Vertices.Reserve(NumQuads * 4);
		IndexBuffer.Indices.Reserve(NumQuads * 6);

		// Same LWC-safe world-planar UV construction as FVoxelChunkSceneProxy
		// (VoxelChunkComponent.cpp) -- see that file's comment for why the
		// combine happens in double before narrowing to float. Water bricks
		// are small (80cm) and comparatively short-lived, but a player-scale
		// pool can still sit far from the world origin, so the same
		// precision care applies.
		const FVector ComponentWorldOrigin = Component->GetComponentLocation();
		constexpr double UVTilePeriodM = 32.0;
		const auto WrapWorldToUV = [](double ComponentOriginUU, double LocalOffsetUU) -> float
		{
			const double WorldM = (ComponentOriginUU + LocalOffsetUU) / 100.0;
			return float(FMath::Fmod(WorldM, UVTilePeriodM));
		};

		for (const FVoxelChunkQuad& Q : Component->ChunkQuads)
		{
			const int32 Axis = Q.Axis;
			const int32 U = (Axis + 1) % 3;
			const int32 V = (Axis + 2) % 3;
			const float FaceCoordVox = float(Q.Slice) + (Q.Positive ? 1.f : 0.f);
			const float U0 = float(Q.U0), V0 = float(Q.V0);
			const float U1 = U0 + float(Q.W), V1 = V0 + float(Q.H);

			auto MakePos = [&](float Uc, float Vc) -> FVector3f
			{
				FVector3f P;
				P[Axis] = FaceCoordVox;
				P[U] = Uc;
				P[V] = Vc;
				return P * VoxelSizeUU;
			};

			// Corner order / AO packing / winding: identical convention to
			// FVoxelChunkSceneProxy (voxelcore/mesher.h's own doc comment is
			// the source of truth both proxies follow).
			FVector3f Pos[4] = {MakePos(U0, V0), MakePos(U0, V1), MakePos(U1, V1), MakePos(U1, V0)};

			const uint8 Ao00 = Q.Ao & 0x3;
			const uint8 Ao10 = (Q.Ao >> 2) & 0x3;
			const uint8 Ao01 = (Q.Ao >> 4) & 0x3;
			const uint8 Ao11 = (Q.Ao >> 6) & 0x3;
			const uint8 AoAtVert[4] = {Ao00, Ao01, Ao11, Ao10};

			const FVector3f Normal = AxisDir[Axis] * (Q.Positive ? 1.f : -1.f);
			const FVector3f TangentX = AxisDir[U];
			const FVector3f TangentY = AxisDir[V];

			const int32 BaseVertex = Vertices.Num();
			for (int32 CornerIdx = 0; CornerIdx < 4; ++CornerIdx)
			{
				FDynamicMeshVertex Vert;
				Vert.Position = Pos[CornerIdx];
				Vert.SetTangents(TangentX, TangentY, Normal);

				const float WorldU = WrapWorldToUV(ComponentWorldOrigin[U], double(Pos[CornerIdx][U]));
				const float WorldV = WrapWorldToUV(ComponentWorldOrigin[V], double(Pos[CornerIdx][V]));
				Vert.TextureCoordinate[0] = FVector2f(WorldU, WorldV);

				// R = material id (always a fixed nonzero placeholder for
				// water, see UVoxelWaterSubsystem.cpp's meshing sampler), G =
				// AO (2-bit -> 0/85/170/255), B unused, A = 255 -- same
				// vertex-color convention as terrain's proxy, so the water
				// material can reuse the same AO-shading approach if wanted.
				const uint8 AoByte = uint8(AoAtVert[CornerIdx] * 85);
				Vert.Color = FColor(Q.Mat, AoByte, 0, 255);

				Vertices.Add(Vert);
			}

			// WINDING -- corrected 2026-07-21 to match FVoxelChunkSceneProxy.
			//
			// This was a copy of the terrain proxy's winding from BEFORE that
			// proxy's inversion bug was found: BOTH branches were flipped, so
			// every water quad was wound with its BACK side front-facing. The
			// terrain fix (see VoxelChunkComponent.cpp, "WINDING -- corrected")
			// established that the correct convention is NEGATIVE faces
			// reversed relative to the naive loop order, not positive ones.
			//
			// Symptom on water specifically: an ocean/pond surface seen from
			// above was showing the underside quad (-Z, lit by the sky through
			// the water normal) and vice versa. It is less obvious than the
			// terrain case because the top and bottom of a water slab are
			// geometrically coincident planes, but the shading normal handed to
			// M_Ocean was inverted, so specular/fresnel responded to a normal
			// pointing into the water rather than out of it.
			if (!Q.Positive)
			{
				IndexBuffer.Indices.Add(BaseVertex + 0);
				IndexBuffer.Indices.Add(BaseVertex + 2);
				IndexBuffer.Indices.Add(BaseVertex + 1);
				IndexBuffer.Indices.Add(BaseVertex + 0);
				IndexBuffer.Indices.Add(BaseVertex + 3);
				IndexBuffer.Indices.Add(BaseVertex + 2);
			}
			else
			{
				IndexBuffer.Indices.Add(BaseVertex + 0);
				IndexBuffer.Indices.Add(BaseVertex + 1);
				IndexBuffer.Indices.Add(BaseVertex + 2);
				IndexBuffer.Indices.Add(BaseVertex + 0);
				IndexBuffer.Indices.Add(BaseVertex + 2);
				IndexBuffer.Indices.Add(BaseVertex + 3);
			}
		}

		// -VoxelWindingCheck: geometric proof that the winding above agrees
		// with the shading normals, and (run against both proxies) that water
		// agrees with terrain.
		//
		// Worth more than a screenshot here. A screenshot of water is easy to
		// misread -- the ocean plane, the translucent material and the sky are
		// all similar blue, and the top and bottom of a water slab are
		// coincident planes -- whereas the sign of
		// dot(cross(P1-P0, P2-P0), N) over the emitted triangles answers the
		// question directly and cannot be argued with. Terrain's convention was
		// verified on screen when its winding was fixed, so "water reports the
		// same sign as terrain" is the verification.
		{
			static const bool bWindingCheck = FParse::Param(FCommandLine::Get(), TEXT("VoxelWindingCheck"));
			if (bWindingCheck && IndexBuffer.Indices.Num() >= 3)
			{
				double DotSum = 0.0;
				int32 TriCount = 0;
				for (int32 I = 0; I + 2 < IndexBuffer.Indices.Num(); I += 3)
				{
					const FVector3f& P0 = Vertices[IndexBuffer.Indices[I + 0]].Position;
					const FVector3f& P1 = Vertices[IndexBuffer.Indices[I + 1]].Position;
					const FVector3f& P2 = Vertices[IndexBuffer.Indices[I + 2]].Position;
					const FVector3f Geo = FVector3f::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();
					// TangentZ is the shading normal stored by SetTangents.
					const FVector3f N = Vertices[IndexBuffer.Indices[I]].TangentZ.ToFVector3f();
					DotSum += double(FVector3f::DotProduct(Geo, N));
					++TriCount;
				}
				UE_LOG(LogTemp, Log,
				       TEXT("VoxelWindingCheck WATER: tris=%d meanDot(geometricNormal, shadingNormal)=%+.3f"),
				       TriCount, TriCount > 0 ? DotSum / double(TriCount) : 0.0);
			}
		}

		VertexBuffers.InitFromDynamicVertex(&VertexFactory, Vertices);

		BeginInitResource(&VertexBuffers.PositionVertexBuffer);
		BeginInitResource(&VertexBuffers.StaticMeshVertexBuffer);
		BeginInitResource(&VertexBuffers.ColorVertexBuffer);
		BeginInitResource(&IndexBuffer);
		BeginInitResource(&VertexFactory);

		Material = Component->ChunkMaterial;
		if (Material == nullptr)
		{
			Material = UMaterial::GetDefaultMaterial(MD_Surface);
		}
		MaterialRelevance = Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
	}

	virtual ~FWaterChunkSceneProxy() override
	{
		VertexBuffers.PositionVertexBuffer.ReleaseResource();
		VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
		VertexBuffers.ColorVertexBuffer.ReleaseResource();
		IndexBuffer.ReleaseResource();
		VertexFactory.ReleaseResource();
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
	                                     uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_WaterChunkSceneProxy_GetDynamicMeshElements);

		const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;

		FMaterialRenderProxy* MaterialProxy = nullptr;
		if (bWireframe)
		{
			auto WireframeMaterialInstance = new FColoredMaterialRenderProxy(
				GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
				FLinearColor(0.1f, 0.3f, 1.f));
			Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
			MaterialProxy = WireframeMaterialInstance;
		}
		else
		{
			MaterialProxy = Material->GetRenderProxy();
		}

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if (VisibilityMap & (1 << ViewIndex))
			{
				const FSceneView* View = Views[ViewIndex];

				FMeshBatch& Mesh = Collector.AllocateMesh();
				FMeshBatchElement& BatchElement = Mesh.Elements[0];
				BatchElement.IndexBuffer = &IndexBuffer;
				Mesh.bWireframe = bWireframe;
				Mesh.VertexFactory = &VertexFactory;
				Mesh.MaterialRenderProxy = MaterialProxy;

				FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer =
					Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
				FPrimitiveUniformShaderParametersBuilder Builder;
				BuildUniformShaderParameters(Builder);
				DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), Builder);
				BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;

				BatchElement.FirstIndex = 0;
				BatchElement.NumPrimitives = IndexBuffer.Indices.Num() / 3;
				BatchElement.MinVertexIndex = 0;
				BatchElement.MaxVertexIndex = VertexBuffers.PositionVertexBuffer.GetNumVertices() - 1;
				Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
				Mesh.Type = PT_TriangleList;
				Mesh.DepthPriorityGroup = SDPG_World;
				Mesh.bCanApplyViewModeOverrides = false;
				Collector.AddMesh(ViewIndex, Mesh);
			}
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bDynamicRelevance = true;
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;
		return Result;
	}

	virtual bool CanBeOccluded() const override { return !MaterialRelevance.bDisableDepthTest; }

	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

	uint32 GetAllocatedSize() const { return FPrimitiveSceneProxy::GetAllocatedSize(); }

private:
	UMaterialInterface* Material = nullptr;
	FStaticMeshVertexBuffers VertexBuffers;
	FDynamicMeshIndexBuffer32 IndexBuffer;
	FLocalVertexFactory VertexFactory;
	FMaterialRelevance MaterialRelevance;
};

// UWaterChunkComponent -------------------------------------------------------

UWaterChunkComponent::UWaterChunkComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;

	// Same reasoning as UVoxelChunkComponent: collision is the terrain DDA
	// raycast, not Chaos -- and v0 active water has no collision of its own
	// at all (force-field buoyancy consumption is W4).
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);
}

void UWaterChunkComponent::SetChunkQuads(TArray<FVoxelChunkQuad>&& InQuads)
{
	ChunkQuads = MoveTemp(InQuads);
	MarkRenderStateDirty();
	UpdateBounds();
}

FPrimitiveSceneProxy* UWaterChunkComponent::CreateSceneProxy()
{
	if (ChunkQuads.Num() == 0)
	{
		return nullptr;
	}
	return new FWaterChunkSceneProxy(this);
}

FBoxSphereBounds UWaterChunkComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const float Extent = float(kBrickEdgeVoxels) * float(VoxelCoords::VoxelSizeUU);
	const FBox LocalBox(FVector::ZeroVector, FVector(Extent, Extent, Extent));
	return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
}

UMaterialInterface* UWaterChunkComponent::GetMaterial(int32 ElementIndex) const
{
	return ElementIndex == 0 ? ChunkMaterial : nullptr;
}

void UWaterChunkComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* NewMaterial)
{
	if (ElementIndex != 0 || ChunkMaterial == NewMaterial)
	{
		return;
	}
	ChunkMaterial = NewMaterial;
	MarkRenderStateDirty();
}

int32 UWaterChunkComponent::GetNumMaterials() const
{
	return 1;
}

void UWaterChunkComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	Super::GetUsedMaterials(OutMaterials, bGetDebugMaterials);
	if (ChunkMaterial)
	{
		OutMaterials.Add(ChunkMaterial);
	}
}
