#include "VoxelChunkComponent.h"

#include "VoxelCoords.h"
#include "VoxelEarth.h" // LogVoxelEarth, for the voxel.GI.Debug 2 per-chunk shading dump
// Read-only reference to UVoxelWorldSubsystem::RingPresets (M2 "Transitions"
// row: the ring cross-fade table is "derived from the ring radii" -- single
// source of truth rather than a second hardcoded copy of 64/128/256/512/1024m).
// FILE OWNERSHIP: this component only reads RingPresets (never writes);
// VoxelWorldSubsystem.* itself is not touched by this change. Safe to include
// from a .cpp regardless of the UHT-parsed-header voxel-core-free doctrine
// (VoxelWorldSubsystem.h is itself voxel-core-free -- PImpl, see its own
// top-of-file comment).
#include "VoxelWorldSubsystem.h"
// M4 voxel light field + cone-traced GI. Both are read-only from here: the
// proxy samples the field to shade, and SetChunkQuads notifies the subsystem
// that this chunk's geometry changed. Default off (voxel.GI.Enabled 0), in
// which case every code path below collapses to exactly what it was.
#include "VoxelGI.h"
#include "VoxelLightField.h"
// Biome appearance: climate -> VertexColor.B/A, decoded by M_VoxelTerrain
// through T_VoxelBiomeLUT. Shared with AVoxelClipmapActor so the near field
// and the 50 km vista cannot drift apart at their seam.
#include "VoxelClimateProbe.h"

#include <atomic>

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
#include "RHICommandList.h"    // FRHICommandListBase::LockBuffer, for the GI colour fast path
#include "RenderingThread.h"   // ENQUEUE_RENDER_COMMAND, ditto
#include "SceneInterface.h"
#include "SceneView.h"
#include "StaticMeshResources.h"

// Shared chunk vertex builder ----------------------------------------------
//
// ONE loop produces the chunk's vertices, indices and vertex colours. The
// scene proxy constructor calls it for everything; the M4 GI colour refresh
// (UVoxelChunkComponent::UpdateGIVertexColors) calls it for colours ALONE and
// memcpys the result straight into the live colour vertex buffer.
//
// WHY THIS FACTORING IS THE SAFETY ARGUMENT. Updating a vertex buffer in place
// is only correct if colour #N still belongs to vertex #N. Rather than assert
// that two similar-looking loops agree, there is exactly one loop: the emission
// order, the greedy-quad subdivision, the DebugVis face-drop `continue`s and
// the degenerate-triangle fallback are all shared code, so the correspondence
// holds by construction rather than by review. -VoxelGIColorCheck then verifies
// it at runtime anyway (see UpdateGIVertexColors).
//
// Colours-only mode skips the work that cannot affect a colour -- tangents, the
// double-precision world-planar UV wrap, index emission and the vertex array
// itself -- but takes every branch that affects HOW MANY vertices are emitted.
namespace
{
	struct FVoxelChunkBuildStats
	{
		bool bGIEnabled = false;
		bool bHasField = false;
		int32 GIHits = 0;
		float GISum = 0.f;
		// Per-face-DIRECTION accounting for voxel.GI.Debug 3 / voxel.GI.DebugVis:
		// bucket index = Axis * 2 + (Positive ? 0 : 1), i.e. +X,-X,+Y,-Y,+Z,-Z.
		// The roof-underside defect was invisible in the whole-chunk aggregate (a
		// roof chunk's bright top faces and dark underside average to a plausible
		// middle) and obvious the moment the buckets were separated.
		int32 DirCount[6] = {};
		int32 DirHits[6] = {};
		float DirIrrSum[6] = {};
		float DirShadeSum[6] = {};
	};

	// Any of the three outputs may be null. OutVertices/OutIndices null means
	// "colours only". All supplied arrays are Reset() first.
	void BuildChunkVertexData(
		const UVoxelChunkComponent& Component,
		TArray<FDynamicMeshVertex>* OutVertices,
		TArray<uint32>* OutIndices,
		TArray<FColor>* OutColors,
		FVoxelChunkBuildStats& OutStats)
	{
		static const FVector3f AxisDir[3] = {FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1)};

		const bool bWantGeometry = (OutVertices != nullptr);
		if (OutVertices) { OutVertices->Reset(); }
		if (OutIndices) { OutIndices->Reset(); }
		if (OutColors) { OutColors->Reset(); }

		const TArray<FVoxelChunkQuad>& ChunkQuads = Component.GetChunkQuads();
		const int32 ChunkLevel = Component.GetLevel();

		// M2 mip rings (docs/m2-plan.md decisions table): "position scale =
		// VoxelSizeUU << level" -- ChunkQuads stay in level-relative voxel
		// units (0..31, baked by MeshChunkBricks); this is the one place that
		// converts them to world-space UU, so it is the one place the level
		// scale needs to apply.
		const float LevelVoxelSizeUU = float(VoxelCoords::VoxelSizeUU) * float(1 << ChunkLevel);

		const int32 NumQuads = ChunkQuads.Num();
		if (OutVertices) { OutVertices->Reserve(NumQuads * 4); }
		if (OutIndices) { OutIndices->Reserve(NumQuads * 6); }
		if (OutColors) { OutColors->Reserve(NumQuads * 4); }

		// World-planar UV origin (docs deliverable: "UV = world-planar
		// (position on the two in-plane axes / 100.0f)"). Component. accessors
		// are game-thread accessors, safe here because both call sites
		// (CreateSceneProxy and UpdateGIVertexColors) run on the game thread;
		// GetLocalToWorld() on the proxy itself is NOT valid at construction
		// time (it is only populated later, when the primitive is added).
		const FVector ComponentWorldOrigin = Component.GetComponentLocation();

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

		// --- M4 voxel GI hookup ------------------------------------------
		//
		// GI is folded into the SAME VertexColor.G byte the mesher's geometric
		// AO already writes, because M_VoxelTerrain computes
		// BaseColor = albedoTint * VertexColor.G * DebugTint. That means no
		// material asset change is needed, and -- critically for the M1 gate --
		// when voxel.GI.Enabled is 0 every line below is skipped and the emitted
		// FColor is byte-identical to what it was before this feature existed.
		//
		// The two terms compose rather than compete: the mesher's per-corner AO
		// is CONTACT-scale occlusion (the inside of a 10cm crease), which a
		// cone trace whose first step is already 40cm wide cannot resolve; the
		// cone trace supplies the LARGE-scale term (a tunnel, a roof, a
		// canyon) that per-voxel AO cannot see. Final G is their product.
		const bool bGIEnabled = VoxelGI::IsEnabled() && ChunkLevel == 0;
		const UVoxelGISubsystem* GISubsystem = nullptr;
		if (bGIEnabled)
		{
			if (const UWorld* World = Component.GetWorld())
			{
				GISubsystem = World->GetSubsystem<UVoxelGISubsystem>();
			}
		}
		const FVoxelLightField* GIField = GISubsystem ? &GISubsystem->GetField() : nullptr;
		TUniquePtr<FVoxelLightField::FReadScope> GIRead;
		if (GIField)
		{
			// One read lock for the whole build instead of one per vertex --
			// a chunk can be several thousand vertices.
			GIRead = MakeUnique<FVoxelLightField::FReadScope>(*GIField);
		}
		const FVector GICentreUU = GISubsystem ? GISubsystem->GetFieldCentreUU() : FVector::ZeroVector;
		const float GIStrength = VoxelGI::GetStrength();
		const float GIAmbientFloor = VoxelGI::GetAmbientFloor();
		const float GIFadeStartUU = VoxelGI::GetFadeStartUU();
		const float GIFadeEndUU = FMath::Max(VoxelGI::GetFadeEndUU(), GIFadeStartUU + 1.f);

		OutStats.bGIEnabled = bGIEnabled;
		OutStats.bHasField = (GIField != nullptr);

		// GI is evaluated per VERTEX, and a greedy quad can span 32 voxels
		// (3.2 m) with only 4 corner samples -- a light pool under a hole in a
		// roof would smear across the whole face. Splitting quads when GI is on
		// buys resolution at the cost of vertices; it is a cvar so the tradeoff
		// is measurable, and it is entirely inert when GI is off.
		const int32 GIMaxSpan = bGIEnabled ? VoxelGI::GetMaxQuadSpanVoxels() : 0;

		const int32 GIDebugVis = bGIEnabled ? VoxelGI::GetDebugVis() : 0;

		// --- surface proximity gate -------------------------------------
		//
		// The biome tint keys off face direction (+Z = sky-facing), because the
		// material ids voxel-core emits here cannot distinguish surface from
		// subsurface -- see VoxelClimateProbe.h. Face direction ALONE is not
		// enough though, and the underground shot proved it: a cave FLOOR is
		// also a +Z face, so the first version painted cave floors grassland
		// green. That is a straight regression underground, which this change
		// is explicitly not allowed to cause.
		//
		// Fix: a +Z face only takes the biome colour if it is actually near the
		// terrain surface. GetSurfaceHeightUU is the full amplifier column (the
		// real surface, not the 30 m tile base), sampled ONCE PER CHUNK at the
		// chunk centre rather than per quad: a chunk is 3.2 m across and the
		// surface barely moves over that, so per-chunk is ample and keeps this
		// off the per-quad path on the meshing workers.
		//
		// Chunks with no subsystem (transient/loading worlds) fall back to
		// "always surface", matching how this behaved before the gate existed.
		double ChunkSurfaceZUU = -TNumericLimits<double>::Max();
		if (const UWorld* SurfWorld = Component.GetWorld())
		{
			if (const UVoxelWorldSubsystem* SurfSub = SurfWorld->GetSubsystem<UVoxelWorldSubsystem>())
			{
				const double HalfChunkUU = 0.5 * double(VoxelCoords::ChunkEdgeVoxels) * double(LevelVoxelSizeUU);
				ChunkSurfaceZUU = SurfSub->GetSurfaceHeightUU(ComponentWorldOrigin.X + HalfChunkUU,
				                                              ComponentWorldOrigin.Y + HalfChunkUU);
			}
		}
		// 2 m of slack: the per-chunk sample is taken at the chunk centre, so a
		// quad at the chunk edge on a steep slope can legitimately sit somewhat
		// below it. Generous enough not to punch holes in the surface, far
		// tighter than the tens of metres a cave sits below it.
		constexpr double kSurfaceBandUU = 200.0;

		// -VoxelMatHistogram: one-shot histogram of the material ids voxel-core
		// actually emits. Added because this change spent a long time reasoning
		// about what biome.h SHOULD produce for these tiles instead of measuring
		// what it DOES; the shader-side symptoms were consistent with several
		// different id distributions and only a direct count separates them.
		// Off unless the switch is present, and it stops after one dump.
		static const bool bMatHistogram = FParse::Param(FCommandLine::Get(), TEXT("VoxelMatHistogram"));
		static std::atomic<int64> MatCounts[16] = {};
		static std::atomic<int64> MatTotal{0};
		static std::atomic<bool> bMatDumped{false};

		for (const FVoxelChunkQuad& Q : ChunkQuads)
		{
			if (bMatHistogram && !bMatDumped.load(std::memory_order_relaxed))
			{
				MatCounts[Q.Mat & 0xF].fetch_add(1, std::memory_order_relaxed);
				if (MatTotal.fetch_add(1, std::memory_order_relaxed) == 2000000)
				{
					bMatDumped.store(true, std::memory_order_relaxed);
					FString Line;
					for (int32 M = 0; M < 16; ++M)
					{
						const int64 C = MatCounts[M].load(std::memory_order_relaxed);
						if (C > 0) { Line += FString::Printf(TEXT("%d=%lld "), M, (long long)C); }
					}
					UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelMatHistogram (2M quads): %s"), *Line);
				}
			}

			const int32 Axis = Q.Axis;
			const int32 U = (Axis + 1) % 3;
			const int32 V = (Axis + 2) % 3;
			const float FaceCoordVox = float(Q.Slice) + (Q.Positive ? 1.f : 0.f);

			auto MakePos = [&](float Uc, float Vc) -> FVector3f
			{
				FVector3f P;
				P[Axis] = FaceCoordVox;
				P[U] = Uc;
				P[V] = Vc;
				return P * LevelVoxelSizeUU;
			};

			// Corner indices match the 2-bits-per-corner AO packing documented
			// in voxelcore/mesher.h: (0,0),(1,0),(0,1),(1,1).
			const float Ao00 = float(Q.Ao & 0x3);
			const float Ao10 = float((Q.Ao >> 2) & 0x3);
			const float Ao01 = float((Q.Ao >> 4) & 0x3);
			const float Ao11 = float((Q.Ao >> 6) & 0x3);

			// Bilinear over the quad's own AO corners, so a subdivided quad
			// reproduces exactly the same AO gradient an unsubdivided one had.
			auto AoAt = [&](float Fu, float Fv) -> float
			{
				return FMath::Lerp(FMath::Lerp(Ao00, Ao10, Fu), FMath::Lerp(Ao01, Ao11, Fu), Fv);
			};

			const FVector3f Normal = AxisDir[Axis] * (Q.Positive ? 1.f : -1.f);
			const FVector3f TangentX = AxisDir[U];
			const FVector3f TangentY = AxisDir[V];

			// DebugVis 4/5: omit one face class entirely. This is the test that
			// settles "which face am I actually looking at" beyond argument --
			// if the surface disappears, it was that class.
			if ((GIDebugVis == 4 && Axis == 2 && Q.Positive) || (GIDebugVis == 5 && Axis == 2 && !Q.Positive))
			{
				continue;
			}

			// --- biome climate, once per quad ------------------------------
			//
			// VertexColor.B = temperature, .A = precipitation, both remapped to
			// this world's measured p1..p99 window (VoxelClimateProbe.h explains
			// why the raw tile bytes are unusable). M_VoxelTerrain feeds them to
			// T_VoxelBiomeLUT as (U=precip, V=temp).
			//
			// PER QUAD, not per vertex: climate is a 30 m raster and a greedy
			// quad spans at most 32 voxels (3.2 m), so per-quad is already ~10x
			// oversampling the source data, at a quarter of the sample cost on
			// the meshing workers. The probe bilinearly filters across raster
			// pixels, so what varies within those 3.2 m is a smooth ramp anyway.
			const FVoxelClimateBytes QuadClimate = VoxelClimate::SampleClimateAtWorldUU(
				ComponentWorldOrigin[0] + double(MakePos(float(Q.U0) + float(Q.W) * 0.5f,
				                                         float(Q.V0) + float(Q.H) * 0.5f)[0]),
				ComponentWorldOrigin[1] + double(MakePos(float(Q.U0) + float(Q.W) * 0.5f,
				                                         float(Q.V0) + float(Q.H) * 0.5f)[1]));

			const int32 SpanU = int32(Q.W);
			const int32 SpanV = int32(Q.H);
			const int32 StepU = (GIMaxSpan > 0) ? FMath::Min(SpanU, GIMaxSpan) : SpanU;
			const int32 StepV = (GIMaxSpan > 0) ? FMath::Min(SpanV, GIMaxSpan) : SpanV;

			for (int32 SubU = 0; SubU < SpanU; SubU += StepU)
			{
				const int32 SubU1 = FMath::Min(SubU + StepU, SpanU);
				for (int32 SubV = 0; SubV < SpanV; SubV += StepV)
				{
					const int32 SubV1 = FMath::Min(SubV + StepV, SpanV);

					const float U0 = float(Q.U0 + SubU), U1 = float(Q.U0 + SubU1);
					const float V0 = float(Q.V0 + SubV), V1 = float(Q.V0 + SubV1);
					// Parametric position of this sub-rect's corners within the
					// parent quad, for the AO bilinear above.
					const float Fu0 = float(SubU) / float(SpanU), Fu1 = float(SubU1) / float(SpanU);
					const float Fv0 = float(SubV) / float(SpanV), Fv1 = float(SubV1) / float(SpanV);

					// Corner order (u0,v0) -> (u0,v1) -> (u1,v1) -> (u1,v0),
					// unchanged from the pre-GI code (the winding below
					// depends on it).
					const FVector3f Pos[4] = {MakePos(U0, V0), MakePos(U0, V1), MakePos(U1, V1), MakePos(U1, V0)};
					const float CornerFu[4] = {Fu0, Fu0, Fu1, Fu1};
					const float CornerFv[4] = {Fv0, Fv1, Fv1, Fv0};

					const int32 BaseVertex = OutVertices ? OutVertices->Num() : 0;
					for (int32 CornerIdx = 0; CornerIdx < 4; ++CornerIdx)
					{
						// R = material id, G = AO * GI, B unused, A = 255.
						const float AoNorm = FMath::Clamp(AoAt(CornerFu[CornerIdx], CornerFv[CornerIdx]) * (85.f / 255.f), 0.f, 1.f);
						float ShadeG = AoNorm;

						const int32 DirBucket = Axis * 2 + (Q.Positive ? 0 : 1);
						bool bGIHit = false;
						float HitIrradiance = 0.f;

						if (GIRead.IsValid())
						{
							const FVector WorldPos = ComponentWorldOrigin + FVector(Pos[CornerIdx]);
							float Irradiance = 0.f;
							// A false return means "the field has nothing solved
							// here yet" (brick streamed in this frame, or we are
							// outside the GI ring). Falling back to plain AO
							// rather than to zero is what stops a chunk from
							// flashing black for the frames before its first
							// solve lands.
							if (GIRead->Sample(WorldPos, Normal, Irradiance))
							{
								bGIHit = true;
								HitIrradiance = Irradiance;
								++OutStats.GIHits;
								OutStats.GISum += Irradiance;
								const float DistUU = float(FVector::Dist(WorldPos, GICentreUU));
								const float Fade = 1.f - FMath::Clamp((DistUU - GIFadeStartUU) / (GIFadeEndUU - GIFadeStartUU), 0.f, 1.f);
								const float Ambient = FMath::Lerp(GIAmbientFloor, 1.f, FMath::Clamp(Irradiance, 0.f, 1.f));
								// Weight 1 -> pure geometric AO, weight 0 -> AO * GI.
								const float Weight = FMath::Clamp(GIStrength * Fade, 0.f, 1.f);
								ShadeG = AoNorm * FMath::Lerp(1.f, Ambient, Weight);
							}
						}

						// Diagnostic override, entirely outside the shipping path
						// (GIDebugVis is 0 unless -VoxelGIVis is on the command
						// line, and is forced to 0 whenever GI is off).
						switch (GIDebugVis)
						{
						case 1: ShadeG = bGIHit ? HitIrradiance : 1.f; break;
						case 2: ShadeG = bGIHit ? 0.f : 1.f; break;
						case 3: ShadeG = FMath::Abs(Normal.Z) > 0.5f ? (Normal.Z < 0.f ? 0.15f : 1.f) : 0.5f; break;
						default: break;
						}

						if (DirBucket >= 0 && DirBucket < 6)
						{
							++OutStats.DirCount[DirBucket];
							OutStats.DirHits[DirBucket] += bGIHit ? 1 : 0;
							OutStats.DirIrrSum[DirBucket] += HitIrradiance;
							OutStats.DirShadeSum[DirBucket] += ShadeG;
						}

						const uint8 ShadeByte = bGIEnabled
							? uint8(FMath::Clamp(FMath::RoundToInt(ShadeG * 255.f), 0, 255))
							// GI off: reproduce the original 2-bit quantisation
							// EXACTLY (0/85/170/255), not a rounded float of it.
							: uint8(int32(AoAt(CornerFu[CornerIdx], CornerFv[CornerIdx]) + 0.5f) * 85);
						// R = biome-tint flag (0 or 255), G = AO * GI,
						// B = temperature, A = precipitation.
						//
						// B and A were both dead bytes until now (B constant 0,
						// A constant 255) -- nothing reads vertex alpha on this
						// material: the M2 ring cross-fade drives OpacityMask
						// from DitherTemporalAA, not from vertex A, so
						// repurposing it costs nothing.
						//
						// R carried the raw Q.Mat until an exposure-proof shader
						// probe showed id thresholds could not be read back
						// reliably; VoxelClimateProbe.h documents the
						// measurement and what it costs. Q.Mat is still the
						// input, it is just resolved to a binary here on the CPU
						// instead of in the shader.
						// Below the surface band, nothing is biome-tinted --
						// cave floors are +Z faces too (see the gate above).
						const bool bNearSurface =
							(ComponentWorldOrigin.Z + double(Pos[CornerIdx].Z)) > (ChunkSurfaceZUU - kSurfaceBandUU);
						const uint8 TintByte = bNearSurface
							? VoxelClimate::BiomeTintForFace(Q.Mat, Axis, Q.Positive != 0)
							: 0;
						const FColor VertColor(TintByte, ShadeByte,
						                       QuadClimate.Temperature, QuadClimate.Precipitation);

						if (OutColors)
						{
							OutColors->Add(VertColor);
						}
						if (bWantGeometry)
						{
							FDynamicMeshVertex Vert;
							Vert.Position = Pos[CornerIdx];
							Vert.SetTangents(TangentX, TangentY, Normal);

							const float WorldU = WrapWorldToUV(ComponentWorldOrigin[U], double(Pos[CornerIdx][U]));
							const float WorldV = WrapWorldToUV(ComponentWorldOrigin[V], double(Pos[CornerIdx][V]));
							Vert.TextureCoordinate[0] = FVector2f(WorldU, WorldV);
							Vert.Color = VertColor;
							OutVertices->Add(Vert);
						}
					}

					// WINDING -- corrected 2026-07-21, and this is the fix for the
					// "roof slab underside renders fully lit" defect.
					//
					// Both branches used to be inverted: with the one-sided
					// M_VoxelTerrain (confirmed at runtime, twoSided=0) every
					// voxel face was wound so that UE treated its BACK side as
					// front-facing. You therefore never saw the face pointing at
					// you -- you saw the far side of the solid, with the shading
					// normal of that far face.
					//
					// On open terrain that mis-renders as "the voxel look":
					// scattered blocks, holes, distant clipmap showing through.
					// Nobody caught it because there was no reference for what it
					// should look like. On the wall+roof fixture it is
					// unmissable: standing under the slab you were shown the
					// slab's SUNLIT TOP face (+Z) instead of its shaded underside
					// (-Z), which is exactly "the underside renders fully lit
					// while the light field correctly reports 0.00 there". The
					// field, the sampling and the vertex colours were all right
					// the whole time -- the wrong triangle was on screen.
					//
					// Proven, not guessed (voxel.GI.DebugVis, this file):
					//   vis 3 (|N.Z| ramp)   -- the ceiling shades as a +Z face.
					//   vis 4 (drop +Z)      -- the ceiling turns to SKY, so the
					//                           -Z underside was drawing nothing.
					//   vis 6 (this winding) -- the ceiling turns black AND the
					//                           terrain becomes a coherent solid.
					//
					// vis 6 is retained as the LEGACY inverted winding so the
					// before/after pair can be captured from one build.
					if (OutIndices)
					{
						const bool bLegacyInvertedWinding = (GIDebugVis == 6);
						const bool bReverseWinding = bLegacyInvertedWinding ? bool(Q.Positive) : !Q.Positive;
						if (bReverseWinding)
						{
							OutIndices->Add(BaseVertex + 0);
							OutIndices->Add(BaseVertex + 2);
							OutIndices->Add(BaseVertex + 1);
							OutIndices->Add(BaseVertex + 0);
							OutIndices->Add(BaseVertex + 3);
							OutIndices->Add(BaseVertex + 2);
						}
						else
						{
							OutIndices->Add(BaseVertex + 0);
							OutIndices->Add(BaseVertex + 1);
							OutIndices->Add(BaseVertex + 2);
							OutIndices->Add(BaseVertex + 0);
							OutIndices->Add(BaseVertex + 2);
							OutIndices->Add(BaseVertex + 3);
						}
					}
				}
			}
		}

		// Only reachable from the DebugVis 4/5 face-drop modes (CreateSceneProxy
		// already refuses to build a proxy for a chunk with no quads, so the
		// shipping path can never land here). The RHI fatals on a zero-sized
		// buffer, so emit one degenerate triangle instead. Colours-only mode
		// must reproduce this too or its count would not match the buffer.
		const int32 EmittedVerts = OutColors ? OutColors->Num() : (OutVertices ? OutVertices->Num() : 0);
		if (EmittedVerts == 0)
		{
			if (OutVertices)
			{
				OutVertices->Reset();
				FDynamicMeshVertex Dummy;
				Dummy.Position = FVector3f::ZeroVector;
				Dummy.SetTangents(FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1));
				Dummy.Color = FColor(0, 0, 0, 0);
				OutVertices->Add(Dummy);
				OutVertices->Add(Dummy);
				OutVertices->Add(Dummy);
			}
			if (OutColors)
			{
				OutColors->Reset();
				OutColors->Add(FColor(0, 0, 0, 0));
				OutColors->Add(FColor(0, 0, 0, 0));
				OutColors->Add(FColor(0, 0, 0, 0));
			}
			if (OutIndices)
			{
				OutIndices->Reset();
				OutIndices->Add(0);
				OutIndices->Add(1);
				OutIndices->Add(2);
			}
		}
	}
}

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
		// ONE shared builder produces geometry and colours (see
		// BuildChunkVertexData above). The GI colour refresh path calls the
		// same function for colours alone, which is what makes an in-place
		// colour-buffer update safe.
		FVoxelChunkBuildStats Stats;
		TArray<FDynamicMeshVertex> Vertices;
		BuildChunkVertexData(*Component, &Vertices, &IndexBuffer.Indices, nullptr, Stats);

		const FVector ComponentWorldOrigin = Component->GetComponentLocation();

		// voxel.GI.Debug 2: per-chunk shading dump. Distinguishes "GI is off",
		// "GI is on but the field had nothing solved here" (hits << verts) and
		// "GI is on and this is what it decided" (meanGI) -- the three
		// explanations for an unexpected-looking chunk, told apart from one log
		// line instead of from a screenshot.
		if (VoxelGI::GetDebugLevel() >= 2 && Vertices.Num() > 0)
		{
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("GIProxy: level=%d origin=(%.0f,%.0f,%.0f) verts=%d giEnabled=%d field=%d hits=%d meanGI=%.3f"),
			       Component->ChunkLevel, ComponentWorldOrigin.X, ComponentWorldOrigin.Y, ComponentWorldOrigin.Z,
			       Vertices.Num(), Stats.bGIEnabled ? 1 : 0, Stats.bHasField ? 1 : 0, Stats.GIHits,
			       Stats.GIHits > 0 ? Stats.GISum / float(Stats.GIHits) : -1.f);
		}

		if (VoxelGI::GetDebugLevel() >= 3 && Stats.bGIEnabled && Vertices.Num() > 0)
		{
			static const TCHAR* kDirNames[6] = {TEXT("+X"), TEXT("-X"), TEXT("+Y"), TEXT("-Y"), TEXT("+Z"), TEXT("-Z")};
			FString Line;
			for (int32 D = 0; D < 6; ++D)
			{
				if (Stats.DirCount[D] == 0)
				{
					continue;
				}
				Line += FString::Printf(TEXT("%s n=%d hit=%d irr=%.3f shade=%.3f | "),
				                        kDirNames[D], Stats.DirCount[D], Stats.DirHits[D],
				                        Stats.DirHits[D] > 0 ? Stats.DirIrrSum[D] / float(Stats.DirHits[D]) : -1.f,
				                        Stats.DirShadeSum[D] / float(Stats.DirCount[D]));
			}
			UE_LOG(LogVoxelEarth, Log, TEXT("GIDir: origin=(%.0f,%.0f,%.0f) %s"),
			       ComponentWorldOrigin.X, ComponentWorldOrigin.Y, ComponentWorldOrigin.Z, *Line);
		}

		// -VoxelWindingCheck: see the matching block in
		// VoxelWaterChunkComponent.cpp. Terrain is the REFERENCE here -- its
		// winding was verified on screen when the inversion was fixed -- so
		// this line exists to be compared against water's.
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
					const FVector3f N = Vertices[IndexBuffer.Indices[I]].TangentZ.ToFVector3f();
					DotSum += double(FVector3f::DotProduct(Geo, N));
					++TriCount;
				}
				UE_LOG(LogTemp, Log,
				       TEXT("VoxelWindingCheck TERRAIN: tris=%d meanDot(geometricNormal, shadingNormal)=%+.3f"),
				       TriCount, TriCount > 0 ? DotSum / double(TriCount) : 0.0);
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

		if (VoxelGI::GetDebugLevel() >= 3)
		{
			static bool bLoggedSidedness = false;
			if (!bLoggedSidedness)
			{
				bLoggedSidedness = true;
				const UMaterial* BaseMat = Material->GetMaterial();
				UE_LOG(LogVoxelEarth, Log, TEXT("GIMat: material=%s twoSided=%d twoSidedRelevance=%d"),
				       *Material->GetName(), BaseMat && BaseMat->IsTwoSided() ? 1 : 0,
				       MaterialRelevance.bTwoSided ? 1 : 0);
			}
		}
	}

	// M4 GI: replace the colour vertex buffer's contents in place ------------
	//
	// THE POINT OF THIS WHOLE CHANGE. GI re-shading used to go through
	// MarkRenderStateDirty(), which destroys this proxy and builds a new one:
	// five fresh RHI buffers, a full re-walk of the quad list to regenerate
	// positions/tangents/UVs/indices that did not change, a scene
	// remove+add of the primitive, and the render-thread deletion of the old
	// proxy. All of that to alter one byte per vertex. Here the geometry is
	// untouched and only the colour stream is overwritten.
	//
	// Safe without any proxy/scene churn because this primitive is
	// DYNAMIC-relevance (GetViewRelevance sets bDynamicRelevance, and drawing
	// goes through GetDynamicMeshElements): there are no cached mesh draw
	// commands referencing the old buffer contents, and vertex colour is not
	// part of GPUScene, so nothing outside this buffer needs invalidating.
	//
	// Returns false if it declined (count/stride mismatch), which the caller
	// turns into a proxy rebuild.
	bool UpdateVertexColors_RenderThread(FRHICommandListBase& RHICmdList, const TArray<FColor>& NewColors)
	{
		check(IsInRenderingThread());

		FColorVertexBuffer& ColorBuffer = VertexBuffers.ColorVertexBuffer;
		const uint32 NumVerts = ColorBuffer.GetNumVertices();

		// The guard that makes the whole scheme safe against races. Between the
		// game thread computing these colours and this command running, the
		// chunk may have been remeshed (different vertex count) or a cvar that
		// changes subdivision (voxel.GI.MaxQuadSpanVoxels, voxel.GI.Enabled)
		// may have moved. Any of those makes the correspondence invalid, and in
		// every such case a proxy rebuild is already on its way, so dropping
		// this update is both safe and correct.
		if (NumVerts == 0 || uint32(NewColors.Num()) != NumVerts || ColorBuffer.GetStride() != sizeof(FColor))
		{
			return false;
		}

		FRHIBuffer* Buffer = ColorBuffer.VertexBufferRHI;
		if (Buffer == nullptr)
		{
			return false; // BeginInitResource has not landed yet
		}

		const uint32 NumBytes = NumVerts * uint32(sizeof(FColor));
		if (void* Dest = RHICmdList.LockBuffer(Buffer, 0, NumBytes, RLM_WriteOnly))
		{
			FMemory::Memcpy(Dest, NewColors.GetData(), NumBytes);
			RHICmdList.UnlockBuffer(Buffer);
			return true;
		}
		return false;
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

// Ring cross-fade table (M2 "Transitions" upgrade) -----------------------
//
// docs/voxel-earth-implementation-plan.md SS3.3: "dithered cross-fade band
// (outer 15-20% of each ring; blue-noise threshold; both LODs rendered in
// band; TSR resolves)". docs/m2-plan.md's "hard boundary v0" decision row is
// what this upgrades -- v0 rendered a hard pop at each ring boundary; this
// adds a per-level material-only fade, no subsystem/streaming change.
namespace
{
	// Inert sentinel Start/End pairs (Tools/create_voxel_material.py mirrors
	// these exactly as the material's own base defaults): a ramp whose
	// Start/End are one of these pairs saturates to 1 (fully opaque) for
	// every realistic in-game camera distance, which is how "no fade on this
	// side" is expressed with the SAME shader formula path used everywhere
	// else (no branch, no special-case node). kInertLow* is used for "ramp is
	// already fully up before any real distance" (disables an inner fade-in);
	// kInertHigh* is "ramp doesn't start falling until far past any real
	// distance" (disables an outer fade-out). Both deltas (1 UU, 1e7 UU) are
	// comfortably representable in float32 at their respective magnitudes, so
	// neither denominator below can round to exactly zero.
	constexpr float kInertLowStart = -2.f, kInertLowEnd = -1.f;
	constexpr float kInertHighStart = 1.e7f, kInertHighEnd = 2.e7f;

	// SEE-THROUGH RING BUG (Matt, 2026-07-24): the M2 cross-dissolve above is
	// broken by construction and is the DOMINANT cause of the "holes" -- whole
	// concentric annuli that read fully transparent and shift as the camera
	// moves. The dither cross-fade only works if the two adjacent LODs OVERLAP
	// spatially so both cover the fade band (plan SS3.3: "both LODs rendered in
	// band"). They do NOT: UVoxelWorldSubsystem::RingPresets annuli ABUT exactly
	// (OuterMeters_L == InnerMeters_{L+1}), zero overlap. So ring L's outer 15%
	// band fades it to fully transparent over real ground that only ring L
	// covers -- ring L+1's inner hole is behind it, not ring L+1 terrain -- and
	// symmetrically for every inner band. The net is a see-through ring at every
	// boundary, ~15% of each ring's width wide (tens of metres, scaling out),
	// concentric to and moving with the camera. Until residency is reworked to
	// make adjacent rings genuinely overlap across the band, the cross-fade is
	// OFF by default: terrain renders opaque and LOD boundaries are hard pops
	// (far less objectionable than holes; the ring-boundary skirt still closes
	// the thin T-junction seam that motivated this branch). -VoxelRingCrossFade
	// re-enables the old fade as an A/B control.
	bool RingCrossFadeEnabled()
	{
		static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelRingCrossFade"));
		return bEnabled;
	}

	// Plan SS3.3: "outer 15-20% of each ring" -- picked the low end of that
	// range (15%) per the task spec; both the inner fade-in and outer
	// fade-out bands use it, each sized against that LEVEL's own annulus
	// width (RingPresets[Level].OuterMeters - InnerMeters), not a shared
	// constant meter width -- so R4's much wider annulus gets a
	// proportionally wider (not narrower) band, matching "each ring" in the
	// plan wording literally.
	constexpr float kFadeBandFraction = 0.15f;

	struct FRingFadeParams
	{
		float InnerStartUU = kInertLowStart, InnerEndUU = kInertLowEnd;
		float OuterStartUU = kInertHighStart, OuterEndUU = kInertHighEnd;
	};

	// docs/m2-plan.md task instructions: "level L fades OUT over its outer
	// 15% band and fades IN over the inner 15% band of its annulus (level 0
	// has no inner fade; the outermost ring keeps no outer fade so the
	// clipmap seam remains hard for now)". Reads UVoxelWorldSubsystem::
	// RingPresets directly (metres) and converts to UU (*100) to match the
	// material's camera-distance space (View.WorldCameraOrigin / Absolute
	// World Position are both UU) -- single source of truth for the ring
	// radii, no second hardcoded copy.
	FRingFadeParams ComputeRingFadeParams(int32 Level)
	{
		FRingFadeParams Params;
		if (!ensure(Level >= 0 && Level < VoxelCoords::kNumLevels))
		{
			return Params; // fully inert on both sides -- never reached in practice
		}
		// See the RingCrossFadeEnabled comment: the abutting-ring cross-fade
		// fades real terrain to transparent, so it is disabled by default. The
		// inert Params (both sides) make OpacityMask saturate to 1 -> opaque.
		if (!RingCrossFadeEnabled())
		{
			return Params;
		}

		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::RingPresets[Level];
		const float InnerUU = float(Preset.InnerMeters * 100.0);
		const float OuterUU = float(Preset.OuterMeters * 100.0);
		const float Band = (OuterUU - InnerUU) * kFadeBandFraction;

		if (Level > 0) // level 0: no inner fade (kInertLow* default stands)
		{
			Params.InnerStartUU = InnerUU;
			Params.InnerEndUU = InnerUU + Band;
		}
		if (Level < VoxelCoords::kNumLevels - 1) // outermost ring: no outer fade (kInertHigh* default stands)
		{
			Params.OuterStartUU = OuterUU - Band;
			Params.OuterEndUU = OuterUU;
		}
		return Params;
	}
}

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

	// SUN SHADOWING -- "the sun should not light a sealed cave" fix.
	//
	// UPrimitiveComponent::CastShadow defaults to FALSE (engine
	// PrimitiveComponent.cpp), and nothing ever set it true on the voxel
	// terrain chunks. The proxy-level shadow flags are all gated on it
	// (PrimitiveSceneProxy.cpp: bCastDynamicShadow = bCastDynamicShadow &&
	// CastShadow && ...), so with CastShadow off FPrimitiveSceneProxy::
	// IsShadowCast() is false, GetViewRelevance() below reports
	// bShadowRelevance=false, and the terrain was NEVER rendered into the
	// directional light's shadow-depth pass. Consequence: the sun passed
	// straight through solid rock, and a cave floor tens of metres underground
	// was lit by the directional sun as if it were outdoors (Control C on the
	// sealed-cave fixture measured the key light on those faces as the sun,
	// 14.5 m under rock). The rock column's topmost +Z surface is a closed
	// caster; once it writes shadow depth, everything beneath it (cave floors,
	// walls, ceilings) is correctly occluded from the sun and is left to the
	// local cave lighting (PR #86 veil/lamp) and voxel GI. The same flag also
	// gives surface relief its sun shadows -- hills shadowing valleys at dawn.
	//
	// Mobility is Movable (never changed away from the UPrimitiveComponent
	// default; VoxelWorldSubsystem.cpp precaches the Movable PSO variant), so
	// this enables the dynamic cascaded-shadow-map path. bCastDynamicShadow is
	// already true by default; CastShadow is the one flag that was suppressing
	// all of it.
	CastShadow = true;
}

void UVoxelChunkComponent::SetChunkQuads(TArray<FVoxelChunkQuad>&& InQuads, int32 InChunkEdgeVoxels)
{
	ChunkQuads = MoveTemp(InQuads);
	ChunkEdgeVoxels = InChunkEdgeVoxels;

	MarkRenderStateDirty();
	UpdateBounds();

	// M4 voxel GI. This single call is the ENTIRE edit-responsiveness
	// mechanism: UVoxelWorldSubsystem already routes both stream-in and
	// every edit-driven remesh through SetChunkQuads, so the GI subsystem
	// learns about a dug tunnel or a blown roof by the same path it learns
	// about a newly streamed chunk -- no deterministic path (worldgen, edit
	// log, digest) is touched or even read to achieve it. With
	// voxel.GI.Enabled 0 this is one cvar read and an immediate return.
	if (const UWorld* World = VoxelGI::IsEnabled() ? GetWorld() : nullptr)
	{
		if (UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
		{
			GI->NotifyChunkMeshUpdated(this);
		}
	}
}

void UVoxelChunkComponent::SetLevel(int32 InLevel)
{
	ChunkLevel = InLevel;
	ApplyRingFadeParams();
}

bool UVoxelChunkComponent::UpdateGIVertexColors()
{
	// No proxy => nothing to update in place (chunk not yet rendered, or it
	// had no quads). Render state already dirty => a fresh proxy is being
	// built this frame and will pick the new field up on its own, so this
	// update would be redundant work on a proxy that is about to die; report
	// success so the caller does not schedule a second rebuild.
	if (!IsRenderStateCreated())
	{
		return false;
	}
	if (IsRenderStateDirty())
	{
		return true;
	}
	FVoxelChunkSceneProxy* Proxy = static_cast<FVoxelChunkSceneProxy*>(SceneProxy);
	if (Proxy == nullptr)
	{
		return false;
	}

	FVoxelChunkBuildStats Stats;
	TArray<FColor> Colors;
	BuildChunkVertexData(*this, nullptr, nullptr, &Colors, Stats);
	if (Colors.Num() == 0)
	{
		return false;
	}

	// -VoxelGIColorCheck: colour-equivalence self-verification, in the spirit
	// of -VoxelWindingCheck.
	//
	// The claim this fast path rests on is "the colours-only walk produces
	// exactly the byte sequence the full proxy build would have written". That
	// is a property of the shared builder, but a property worth MEASURING
	// rather than asserting -- a faster path that subtly changes shading is a
	// regression, and a one-vertex ordering slip would be invisible on screen
	// while corrupting a whole chunk's lighting.
	//
	// So: run the FULL build (geometry + colours, the exact code the proxy
	// constructor runs) against the same field state, and compare its
	// per-vertex colours with the colours-only array element by element. Logs
	// a count, a mismatch tally and an FNV-1a digest of each stream, so a run
	// can be checked by grepping for a single line rather than by eye.
	{
		static const bool bColorCheck = FParse::Param(FCommandLine::Get(), TEXT("VoxelGIColorCheck"));
		if (bColorCheck)
		{
			FVoxelChunkBuildStats FullStats;
			TArray<FDynamicMeshVertex> FullVerts;
			TArray<uint32> FullIndices;
			BuildChunkVertexData(*this, &FullVerts, &FullIndices, nullptr, FullStats);

			auto Digest = [](const auto& Container, auto&& Get) -> uint64
			{
				uint64 Hash = 0xcbf29ce484222325ull;
				for (int32 I = 0; I < Container.Num(); ++I)
				{
					const FColor C = Get(Container[I]);
					const uint8 Bytes[4] = {C.R, C.G, C.B, C.A};
					for (uint8 B : Bytes)
					{
						Hash ^= uint64(B);
						Hash *= 0x100000001b3ull;
					}
				}
				return Hash;
			};

			const uint64 FastDigest = Digest(Colors, [](const FColor& C) { return C; });
			const uint64 FullDigest = Digest(FullVerts, [](const FDynamicMeshVertex& V) { return V.Color; });

			int32 Mismatches = 0;
			const int32 Common = FMath::Min(Colors.Num(), FullVerts.Num());
			for (int32 I = 0; I < Common; ++I)
			{
				if (Colors[I] != FullVerts[I].Color)
				{
					++Mismatches;
				}
			}

			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelGIColorCheck: fastVerts=%d fullVerts=%d mismatches=%d fastDigest=0x%016llX fullDigest=0x%016llX %s"),
			       Colors.Num(), FullVerts.Num(), Mismatches, FastDigest, FullDigest,
			       (Colors.Num() == FullVerts.Num() && Mismatches == 0 && FastDigest == FullDigest) ? TEXT("MATCH") : TEXT("*** DIFFER ***"));
		}
	}

	// Raw proxy pointer across the thread boundary is the established engine
	// pattern for this (see ProceduralMeshComponent's UpdateSection_*): proxy
	// destruction is itself an enqueued render command, so a command enqueued
	// now is guaranteed to run before any deletion enqueued later.
	ENQUEUE_RENDER_COMMAND(FVoxelChunkGIColorUpdate)(
		[Proxy, MovedColors = MoveTemp(Colors)](FRHICommandListImmediate& RHICmdList)
		{
			Proxy->UpdateVertexColors_RenderThread(RHICmdList, MovedColors);
		});

	return true;
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
	if (ElementIndex != 0)
	{
		return;
	}

	// M2: compare against the current BASE (ChunkMID's Parent once one
	// exists, since ChunkMaterial itself becomes the MID -- see ChunkMaterial's
	// doc comment) rather than ChunkMaterial directly, so re-asserting the
	// same base material (e.g. a redundant SetMaterial(0, Material) call) is
	// still a correct no-op instead of spuriously dropping/recreating ChunkMID.
	UMaterialInterface* CurrentBase = ChunkMaterial;
	if (ChunkMID)
	{
		CurrentBase = ChunkMID->Parent;
	}
	if (CurrentBase == NewMaterial)
	{
		return;
	}

	// Base material changed (or this is first assignment): any existing MID
	// was wrapping the OLD base, so it's stale -- drop it, ApplyRingFadeParams
	// below creates a fresh one over NewMaterial.
	ChunkMID = nullptr;
	ChunkMaterial = NewMaterial;
	MarkRenderStateDirty();
	ApplyRingFadeParams();
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
	// M2: ChunkMID is created unconditionally by ApplyRingFadeParams as soon
	// as a base material is known (fades are always-on) -- this no longer
	// creates a MID itself, it only sets a parameter on the one that already
	// exists. If SetMaterial has genuinely never run yet, there's nothing to
	// tint (shouldn't happen in practice: the subsystem always calls
	// SetMaterial right after NewObject, before this could ever be reached).
	if (!ChunkMID)
	{
		return;
	}
	ChunkMID->SetVectorParameterValue(TEXT("DebugTint"), Tint);
	bDebugTintDirty = true; // M1 hitch-gap wave: see ClearDebugTint's early-out
}

void UVoxelChunkComponent::ClearDebugTint()
{
	// M2: reset to the multiplicative identity (opaque white) instead of
	// dropping ChunkMID -- the MID must persist regardless of this layer's
	// on/off state, since it also carries the always-on ring-fade params
	// (see ChunkMID's doc comment). There is no second MID to drop anymore;
	// "zero extra cost when this layer is off" now means "one SetVectorParameterValue
	// call, no MID churn / no MarkRenderStateDirty", which is strictly
	// cheaper than the old create/destroy-MID path, not more expensive.
	//
	// M1 hitch-gap wave (component pooling): ReturnChunkComponentToPool now
	// calls this unconditionally on EVERY pool-park, which -- unlike
	// SetVisibility/SetChunkQuads's MarkRenderStateDirty -- does NOT coalesce
	// with anything else; SetVectorParameterValue fires its own immediate
	// render-thread command every time it's actually called. Measured impact:
	// with this early-out ABSENT, a min-spec-proxy -VoxelPerfRun=60 gate run
	// (voxel.Debug=0 the whole time, i.e. DebugTint was never touched by
	// anything) still enqueued this render command on every one of the
	// steady ~2 unloads/frame during ring-crossing churn -- pure waste, and a
	// real regression vs the pre-pooling DestroyComponent path (which never
	// touched a material parameter on unload at all). bDebugTintDirty tracks
	// "does this MID's DebugTint param currently differ from the identity",
	// so a component that was never tinted (the overwhelmingly common case
	// whenever voxel.Debug's visualization layers are off) skips the call
	// entirely -- zero render-thread cost, not just zero EXTRA MID churn.
	if (!ChunkMID || !bDebugTintDirty)
	{
		return;
	}
	ChunkMID->SetVectorParameterValue(TEXT("DebugTint"), FLinearColor::White);
	bDebugTintDirty = false;
}

void UVoxelChunkComponent::ApplyRingFadeParams()
{
	// Whichever of SetLevel/SetMaterial runs second is the one that actually
	// has both pieces of information -- both call this, so the no-op on the
	// first call (missing base material or, in principle, missing level) is
	// expected, not an error.
	UMaterialInterface* Base = ChunkMaterial;
	if (ChunkMID)
	{
		Base = ChunkMID->Parent;
	}
	if (!Base)
	{
		return; // no base material assigned yet (SetLevel runs before SetMaterial)
	}

	if (!ChunkMID)
	{
		// UPrimitiveComponent (this component's base, not UMeshComponent) has
		// no CreateDynamicMaterialInstance helper -- hand-rolled equivalent,
		// same pattern the old SetDebugTint used: UMaterialInstanceDynamic::Create
		// over the current base, then this component's ChunkMaterial field
		// (which CreateSceneProxy/GetMaterial/GetUsedMaterials all read) takes
		// over as the MID from here on.
		ChunkMID = UMaterialInstanceDynamic::Create(Base, this);
		if (!ChunkMID)
		{
			return;
		}
	}

	const FRingFadeParams Params = ComputeRingFadeParams(ChunkLevel);
	ChunkMID->SetScalarParameterValue(TEXT("RingInnerFadeStart"), Params.InnerStartUU);
	ChunkMID->SetScalarParameterValue(TEXT("RingInnerFadeEnd"), Params.InnerEndUU);
	ChunkMID->SetScalarParameterValue(TEXT("RingOuterFadeStart"), Params.OuterStartUU);
	ChunkMID->SetScalarParameterValue(TEXT("RingOuterFadeEnd"), Params.OuterEndUU);

	if (ChunkMaterial != ChunkMID)
	{
		ChunkMaterial = ChunkMID;
		MarkRenderStateDirty();
	}
}
