#include "VoxelChunkComponent.h"

#include "VoxelCoords.h"

#include "DynamicMeshBuilder.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveDrawingUtils.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "PrimitiveViewRelevance.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "StaticMeshResources.h"

// FVoxelChunkSceneProxy -----------------------------------------------------
//
// Doctrine (docs/voxel-earth-implementation-plan.md SS3.3 Band 1): hand-rolled
// FPrimitiveSceneProxy, NOT ProceduralMeshComponent / UDynamicMesh. Structure
// (buffers / InitResources / GetDynamicMeshElements) follows the engine's
// CustomMeshComponent plugin 1:1 -- see
// Engine/Plugins/Runtime/CustomMeshComponent/Source/CustomMeshComponent/Private/CustomMeshComponent.cpp
// in the installed UE 5.7 -- with quads (4 verts / 6 indices) in place of
// its raw triangle list, and per-vertex color/normal driven by voxel-core's
// greedy-mesher output instead of a flat white color.
class FVoxelChunkSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	explicit FVoxelChunkSceneProxy(UVoxelChunkComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel(), "FVoxelChunkSceneProxy")
	{
		static const FVector3f AxisDir[3] = {FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1)};

		// M2 mip rings (docs/m2-plan.md decisions table): "position scale =
		// VoxelSizeUU << level" -- ChunkQuads stay in level-relative voxel
		// units (0..31, baked by MeshChunkBricks); this is the one place that
		// converts them to world-space UU, so it is the one place the level
		// scale needs to apply.
		const float LevelVoxelSizeUU = float(VoxelCoords::VoxelSizeUU) * float(1 << Component->ChunkLevel);

		const int32 NumQuads = Component->ChunkQuads.Num();
		TArray<FDynamicMeshVertex> Vertices;
		Vertices.Reserve(NumQuads * 4);
		IndexBuffer.Indices.Reserve(NumQuads * 6);

		// World-planar UV origin (docs deliverable: "UV = world-planar
		// (position on the two in-plane axes / 100.0f)"). Component-> is a
		// game-thread accessor, safe here because CreateSceneProxy (which
		// constructs this proxy) runs on the game thread; GetLocalToWorld()
		// on the proxy itself is NOT valid yet at construction time (it is
		// only populated later, when the primitive is added to the scene).
		const FVector ComponentWorldOrigin = Component->GetComponentLocation();

		// Stage 3c LWC precision (docs/m1-plan.md stage 3c; plan SS3.3): near
		// Earth-scale coordinates (|X| ~ 2e8 UU at 2,000km from the world
		// origin) a float has ~16 UU (~16cm) of representable step, so naively
		// narrowing ComponentWorldOrigin to float before combining with the
		// local vertex offset (as this used to do) loses enough precision to
		// shimmer the world-planar UVs under camera motion. Fix: do the
		// combine in double, then reduce to a small tiling period in double
		// BEFORE narrowing to float -- the value that ever reaches a float is
		// always small (sub-mm precision everywhere), regardless of how far
		// the chunk is from the origin. ChunkEdgeUU (320 UU = 3.2m) divides
		// UVTilePeriodM evenly, so vertices shared across a chunk border still
		// wrap identically (same double input -> same output) and no new
		// seams appear. The material's world-planar look is unchanged: UV
		// scale is identical, the pattern now simply repeats every
		// UVTilePeriodM instead of never (it already effectively repeated
		// every ~168m where the old float precision wrapped around anyway).
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
				return P * LevelVoxelSizeUU;
			};

			// Quad loop (u0,v0) -> (u0,v1) -> (u1,v1) -> (u1,v0); corner
			// indices below match the 2-bits-per-corner AO packing documented
			// in voxelcore/mesher.h: (0,0),(1,0),(0,1),(1,1).
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

				// R = material id, G = AO (2-bit -> 0/85/170/255), B unused,
				// A = 255 (deliverable 3 vertex-color spec).
				const uint8 AoByte = uint8(AoAtVert[CornerIdx] * 85);
				Vert.Color = FColor(Q.Mat, AoByte, 0, 255);

				Vertices.Add(Vert);
			}

			// Winding: verified empirically (wireframe-visible / lit-invisible
			// on the original orientation, 2026-07-19): UE front faces need
			// the loop order REVERSED for Positive faces relative to the
			// initial guess. Positive and negative faces still wind
			// oppositely, and the mesh is now correct with a one-sided
			// material (the temporary two-sided material flag can go away).
			if (Q.Positive)
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

		VertexBuffers.InitFromDynamicVertex(&VertexFactory, Vertices);

		// Enqueue initialization of render resources (matches
		// CustomMeshComponent.cpp exactly).
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

	virtual ~FVoxelChunkSceneProxy() override
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
		QUICK_SCOPE_CYCLE_COUNTER(STAT_VoxelChunkSceneProxy_GetDynamicMeshElements);

		const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;

		FMaterialRenderProxy* MaterialProxy = nullptr;
		if (bWireframe)
		{
			auto WireframeMaterialInstance = new FColoredMaterialRenderProxy(
				GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
				FLinearColor(0, 0.5f, 1.f));
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

// UVoxelChunkComponent --------------------------------------------------

UVoxelChunkComponent::UVoxelChunkComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;

	// Terrain collision is a custom DDA raycast/box-sweep against the brick
	// grid (docs/voxel-earth-implementation-plan.md SS3.3), not Chaos -- this
	// render-only primitive must not participate in Chaos collision.
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);
}

void UVoxelChunkComponent::SetChunkQuads(TArray<FVoxelChunkQuad>&& InQuads, int32 InChunkEdgeVoxels)
{
	ChunkQuads = MoveTemp(InQuads);
	ChunkEdgeVoxels = InChunkEdgeVoxels;

	MarkRenderStateDirty();
	UpdateBounds();
}

FPrimitiveSceneProxy* UVoxelChunkComponent::CreateSceneProxy()
{
	if (ChunkQuads.Num() == 0)
	{
		return nullptr;
	}
	return new FVoxelChunkSceneProxy(this);
}

FBoxSphereBounds UVoxelChunkComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// M2 mip rings: bounds must scale with this chunk's level too (see
	// SetLevel doc comment) or a coarse-level chunk's true (larger) world
	// footprint gets culled/frustum-tested against its level-0-sized bounds.
	const float Extent = float(ChunkEdgeVoxels) * float(VoxelCoords::VoxelSizeUU) * float(1 << ChunkLevel);
	const FBox LocalBox(FVector::ZeroVector, FVector(Extent, Extent, Extent));
	return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
}

UMaterialInterface* UVoxelChunkComponent::GetMaterial(int32 ElementIndex) const
{
	return ElementIndex == 0 ? ChunkMaterial : nullptr;
}

void UVoxelChunkComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* NewMaterial)
{
	if (ElementIndex == 0 && ChunkMaterial != NewMaterial)
	{
		ChunkMaterial = NewMaterial;
		MarkRenderStateDirty();
	}
}

int32 UVoxelChunkComponent::GetNumMaterials() const
{
	return 1;
}

void UVoxelChunkComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	Super::GetUsedMaterials(OutMaterials, bGetDebugMaterials);
	if (ChunkMaterial)
	{
		OutMaterials.Add(ChunkMaterial);
	}
}

void UVoxelChunkComponent::SetDebugTint(const FLinearColor& Tint)
{
	if (!DebugTintMID)
	{
		// UPrimitiveComponent (this component's base, not UMeshComponent) has
		// no CreateDynamicMaterialInstance helper -- hand-rolled equivalent:
		// create the MID over whatever material is currently assigned, then
		// route it through SetMaterial (the same virtual UPrimitiveComponent
		// calls internally) so ChunkMaterial and the scene proxy both pick it
		// up via the normal MarkRenderStateDirty path.
		UMaterialInterface* Base = ChunkMaterial;
		if (!Base)
		{
			return; // no material assigned yet; nothing to tint
		}
		DebugTintMID = UMaterialInstanceDynamic::Create(Base, this);
		if (!DebugTintMID)
		{
			return;
		}
		SetMaterial(0, DebugTintMID);
	}
	DebugTintMID->SetVectorParameterValue(TEXT("DebugTint"), Tint);
}

void UVoxelChunkComponent::ClearDebugTint()
{
	if (!DebugTintMID)
	{
		return;
	}
	UMaterialInterface* Base = DebugTintMID->Parent;
	DebugTintMID = nullptr;
	SetMaterial(0, Base);
}
