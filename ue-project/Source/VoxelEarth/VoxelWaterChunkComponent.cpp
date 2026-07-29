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
		// SetChunkQuads check()s this, so a mismatch is a caller bug and not a
		// state -- but check() is compiled out of Shipping and the consequence
		// there would be an out-of-bounds read on the render path rather than a
		// wrong picture. The fallback is exactly what R held before per-corner
		// heights existed (the quad's own `mat` byte on all four corners), so a
		// build that somehow lost the array draws the old flat-topped water
		// instead of crashing.
		const bool bHasCornerHeights = Component->ChunkCornerHeights.Num() == NumQuads;
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

		for (int32 QuadIdx = 0; QuadIdx < NumQuads; ++QuadIdx)
		{
			const FVoxelChunkQuad& Q = Component->ChunkQuads[QuadIdx];
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

			// Which of this quad's four corners sit on the TOP (+Z) boundary of
			// the voxel the face belongs to. M_WaterVoxel's World Position
			// Offset lowers exactly those, by (1 - fill) of a voxel, which is
			// what turns a partially-filled cell into a real surface.
			//
			// It has to be per-VERTEX, not per-face: gating on the face normal
			// would lower only +Z faces and leave a partial cell's side walls
			// at full height, ringing every pool with a one-voxel bathtub rim.
			// A side face has two top corners and two bottom ones, so moving
			// only the top pair makes it a trapezoid that meets the surface.
			//
			// For a Z-normal face the whole quad is in one plane, so the test
			// is just Positive. Otherwise the quad spans a Z range and the top
			// corners are the ones at its maximum Z -- read straight off the
			// corner positions rather than re-deriving which of U/V carries Z.
			// Mirrors DecodeVoxelQuadVertex's TopBoundary in VoxelQuadDecode.ush;
			// the two must agree or the pooled and component paths draw
			// different geometry for the same water.
			bool bTopCorner[4] = {false, false, false, false};
			if (Axis == 2)
			{
				const bool bPositiveFace = (Q.Positive != 0);
				bTopCorner[0] = bTopCorner[1] = bTopCorner[2] = bTopCorner[3] = bPositiveFace;
			}
			else
			{
				float MaxZ = Pos[0].Z;
				for (int32 I = 1; I < 4; ++I)
				{
					MaxZ = FMath::Max(MaxZ, Pos[I].Z);
				}
				for (int32 I = 0; I < 4; ++I)
				{
					bTopCorner[I] = Pos[I].Z > MaxZ - 0.5f * VoxelSizeUU;
				}
			}

			// The four per-corner surface heights this quad carries, in the same
			// position order as Pos[] above. See UWaterChunkComponent::
			// SetChunkQuads and UVoxelWaterSubsystem's EmitWaterQuads: a corner on
			// the cell's top boundary holds its bilinear surface height, and every
			// other corner holds the quad's own `mat` byte -- which is exactly what
			// R held for EVERY vertex before per-corner heights existed, so nothing
			// off the water surface changed value.
			const uint32 PackedCorners = bHasCornerHeights
				? Component->ChunkCornerHeights[QuadIdx]
				: (uint32(Q.Mat) * 0x01010101u);
			const uint8 CornerHeight[4] = {
				uint8( PackedCorners        & 0xffu),
				uint8((PackedCorners >>  8) & 0xffu),
				uint8((PackedCorners >> 16) & 0xffu),
				uint8((PackedCorners >> 24) & 0xffu)};

			// SHADING NORMAL FROM THE CORNER HEIGHTS, on the water surface only.
			//
			// The top of a +Z face is no longer flat -- the four corners sit at
			// four different heights and the material's World Position Offset moves
			// them there -- so a hard (0,0,1) normal now describes geometry that is
			// not on screen. At roughness 0.1 that is the difference between a
			// surface that reads as water and one that reads as blue glass tiles:
			// the specular highlight is where the normal says it is, not where the
			// silhouette is.
			//
			// Derived from the quad's own four corners, NOT from a wider stencil of
			// the corner field, and that is a deliberate constraint rather than an
			// approximation of choice: the pooled path has ONLY these four bytes to
			// work with (it reconstructs every vertex from SV_VertexID and one
			// packed word), and the two paths must not shade the same water
			// differently. A true per-vertex normal would need central differences
			// across the brick boundary, i.e. a 2-voxel apron and a second round of
			// vxc::WaterCA::fillAt lookups in the loop this module has already had
			// to make cheap twice. VoxelQuadDecode.ush computes the identical
			// expression; if one moves, both move.
			//
			// Side and bottom faces keep their exact axis normal -- a wall is still
			// a wall, and only its top EDGE moves.
			FVector3f Normal = AxisDir[Axis] * (Q.Positive ? 1.f : -1.f);
			if (Axis == 2 && Q.Positive != 0)
			{
				// Heights are 0..255 within one voxel, so a unit of height is
				// 1/255 of a voxel and a unit of u/v is one whole voxel: the 255
				// below is the conversion, not a normalisation.
				const float dHdU = float((int32(CornerHeight[2]) + int32(CornerHeight[3]))
				                       - (int32(CornerHeight[0]) + int32(CornerHeight[1])))
				                 / (2.f * 255.f * float(Q.W));
				const float dHdV = float((int32(CornerHeight[1]) + int32(CornerHeight[2]))
				                       - (int32(CornerHeight[0]) + int32(CornerHeight[3])))
				                 / (2.f * 255.f * float(Q.H));
				// Axis 2 means u = x and v = y, so this is already world-axis order.
				Normal = FVector3f(-dHdU, -dHdV, 1.f).GetSafeNormal();
			}

			// Gram-Schmidt each in-plane axis against the (possibly tilted) normal,
			// rather than rebuilding the frame from a cross product. Both dot
			// products are exactly zero for an axis-aligned normal, so every face
			// that did not tilt gets back the unit axis vector it had before, bit
			// for bit -- including the HANDEDNESS of the pair, which a
			// cross(Normal, TangentX) would silently invert on negative faces and
			// which FDynamicMeshVertex::SetTangents encodes as the basis sign.
			FVector3f TangentX = AxisDir[U] - Normal * FVector3f::DotProduct(Normal, AxisDir[U]);
			FVector3f TangentY = AxisDir[V] - Normal * FVector3f::DotProduct(Normal, AxisDir[V]);
			TangentX = TangentX.GetSafeNormal(KINDA_SMALL_NUMBER, AxisDir[U]);
			TangentY = TangentY.GetSafeNormal(KINDA_SMALL_NUMBER, AxisDir[V]);

			const int32 BaseVertex = Vertices.Num();
			for (int32 CornerIdx = 0; CornerIdx < 4; ++CornerIdx)
			{
				FDynamicMeshVertex Vert;
				Vert.Position = Pos[CornerIdx];
				Vert.SetTangents(TangentX, TangentY, Normal);

				const float WorldU = WrapWorldToUV(ComponentWorldOrigin[U], double(Pos[CornerIdx][U]));
				const float WorldV = WrapWorldToUV(ComponentWorldOrigin[V], double(Pos[CornerIdx][V]));
				Vert.TextureCoordinate[0] = FVector2f(WorldU, WorldV);

				// R = THIS CORNER's water surface height, 0..255 within the cell
				// the face belongs to (was: the whole face's CA fill fraction,
				// which is why a settled pool read as a field of 10 cm plateaus
				// with open slits between cells of differing fill -- see
				// UVoxelWaterSubsystem.cpp's corner-field comment). G = AO
				// (2-bit -> 0/85/170/255), B = top-boundary flag (see bTopCorner
				// above), A = per-brick foam ACTIVITY (W5, was a fixed 255).
				//
				// A IS PER BRICK, NOT PER VERTEX, and it is the one channel here
				// that the pooled path reaches by a completely different route:
				// there it rides ChunkParams.y, which the vertex factory already
				// copies into colour A for every mode
				// (VoxelQuadVertexFactory.ush). Same value, same 8-bit
				// quantisation, two mechanisms -- so this is the channel to check
				// first if the two paths ever foam differently.
				//
				// R and B together are what M_WaterVoxel's World Position
				// Offset consumes, and the formula is UNCHANGED by per-corner R:
				// it lowers a top-boundary vertex by (1 - R) of a voxel, which
				// simply now varies across the quad instead of being constant on
				// it. Terrain's proxy uses R and B for a biome tint and climate
				// instead, so this convention is water's own rather than shared --
				// the pooled path reproduces THIS one under
				// FVoxelQuadVertexFactoryParameters::WaterMode.
				const uint8 AoByte = uint8(AoAtVert[CornerIdx] * 85);
				Vert.Color = FColor(CornerHeight[CornerIdx], AoByte, bTopCorner[CornerIdx] ? 255 : 0,
				                    Component->ChunkActivity);

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

void UWaterChunkComponent::SetChunkQuads(TArray<FVoxelChunkQuad>&& InQuads, TArray<uint32>&& InCornerHeights,
                                         float InActivity)
{
	// The proxy indexes both arrays with one loop counter. A short corner array
	// would be an out-of-bounds read on the very first frame the mismatch
	// happened; a long one would silently misalign every height. Checked here
	// rather than defended in the proxy so the failure names its producer.
	check(InCornerHeights.Num() == InQuads.Num());
	ChunkQuads = MoveTemp(InQuads);
	ChunkCornerHeights = MoveTemp(InCornerHeights);
	// 255, not 256: an FColor byte round-trips to 1.0 in the shader only at 255,
	// and foam at full activity should reach the material's full-foam end of the
	// lerp exactly rather than 255/256ths of it.
	ChunkActivity = uint8(FMath::Clamp(InActivity, 0.0f, 1.0f) * 255.0f + 0.5f);
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
