// GPU-readable voxel GI light field (docs/gpu-gi-volume-design.md).
//
// The CPU path bakes GI into vertex colour G at MESH time, which is why a dig
// currently re-meshes a chunk partly just to refresh its lighting. Sampling a
// volume by world position instead decouples the two: lighting updates become
// texel uploads and geometry updates stay geometry updates. That decoupling --
// not parity -- is the point of this file.
//
// NO MATERIAL CHANGE IS INVOLVED. M_VoxelTerrain computes
// BaseColor = albedo * VertexColor.G * DebugTint, and the pooled vertex factory
// owns both ends of the vertex-colour pipe (it writes Intermediates.Color in the
// VS and Result.VertexColor in GetMaterialPixelParameters). So the sampled
// irradiance is folded into .g inside the factory and the material graph never
// learns this exists.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "ShaderParameters.h"
#include "RHIResources.h"

// Bound alongside VoxelVF, as a SECOND uniform buffer rather than by widening
// that one. The factory's buffer is built once in InitRHI and its header warns
// that changing an input afterwards needs a re-init; this one changes whenever
// the volume re-centres, and this renderer fails silently, so the two are kept
// separate deliberately.
//
// NAMING: registered shader-side as "VoxelGIVol", so in HLSL these are reached
// as VoxelGIVol.Volume -- NOT as a loose `Texture3D Volume;`. A loose
// declaration compiles perfectly and then reads zeros forever, because it is a
// different, unbound symbol. Same trap the factory's own header documents.
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelGIVolumeParameters, VOXELEARTHSHADERS_API)
	SHADER_PARAMETER_TEXTURE(Texture3D, Volume)
	SHADER_PARAMETER_SAMPLER(SamplerState, VolumeSampler)
	// Volume origin in POOL-PRIMITIVE space, and 1/(N*CellSizeUU) per axis.
	//
	// Pool space is the only correct space for this. At ~8.4M UU float32's ULP
	// is 1.0 UU against a 40 UU cell, and computing WorldPos - VolumeOrigin at
	// that magnitude is catastrophic cancellation. The pool component carries
	// the big offset in its double-precision transform, so Intermediates.Position
	// is already small; supplying the origin in the same space keeps the whole
	// lookup in the range where float32 has ~0.015 UU of headroom.
	SHADER_PARAMETER(FVector3f, OriginPoolUU)
	SHADER_PARAMETER(FVector3f, InvSizeUU)
	// Centre the distance fade is measured FROM, in the same pool space. This is
	// the CAMERA, not the volume centre.
	//
	// They are not the same thing and the difference is not small. The CPU shade
	// (VoxelChunkComponent.cpp, BuildChunkVertexData) measures
	// Dist(WorldPos, GICentreUU) where GICentreUU is the camera; the volume's
	// origin follows the camera only to within the re-centring dead zone
	// (voxel.GI.VolumeRecentreCells, 2560 UU by default), so using the volume
	// centre here would slide the whole fade band by up to that much and put a
	// moving brightness gradient on static geometry. Supplied per frame.
	SHADER_PARAMETER(FVector3f, FadeCentrePoolUU)
	SHADER_PARAMETER(float, Strength)
	SHADER_PARAMETER(float, AmbientFloor)
	SHADER_PARAMETER(float, FadeStartUU)
	SHADER_PARAMETER(float, FadeEndUU)
	// VoxelLF::CellSizeUU, so the shader's fallback probe offsets are expressed
	// in the same units the CPU sampler's {0.6, 1.25, 2.0} cells are, rather
	// than in a second hardcoded 40.
	SHADER_PARAMETER(float, CellSizeUU)
	// 0 = the factory skips the sample entirely and vertex colour G keeps the
	// mesher's AO, byte-identical to today.
	SHADER_PARAMETER(uint32, Enabled)
	// docs/gpu-gi-volume-design.md §9: 3 = world checkerboard. Kept as a
	// permanent rung of the diagnostic ladder rather than deleted after
	// bring-up, because a pooled primitive fails silently and this is the
	// cheapest proof that the whole path is reachable from the pixel shader.
	SHADER_PARAMETER(uint32, DebugVis)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

namespace VoxelGIVolume
{
	// Matches VoxelLF::CellSizeUU. The volume's texel lattice must coincide
	// exactly with the light field's cell lattice or every sample resamples.
	inline constexpr float kCellSizeUU = 40.0f;

	// Per-axis texel count. 64 covers +/-1280 UU (12.8 m) at 1 MB, which is
	// enough to prove the path; the design doc's recommended shipping size is
	// 256 (+/-5120 UU, 67 MB) and is a cvar, not a recompile.
	VOXELEARTHSHADERS_API int32 GetDim();

	// Master switch. Off by default: with it off the factory does not sample,
	// and the emitted vertex colour is byte-identical to what it is today.
	VOXELEARTHSHADERS_API bool IsEnabled();

	VOXELEARTHSHADERS_API int32 GetDebugVis();

	// Dead-zone half-width in CELLS for camera-following re-centring
	// (docs/gpu-gi-volume-design.md §4). Clamped against the volume's own
	// half-extent by the caller: a dead zone wider than the volume would let the
	// camera leave coverage without ever triggering a re-centre, which is the
	// one failure this knob can produce and it looks like "GI stopped working"
	// rather than like a bad setting.
	VOXELEARTHSHADERS_API int32 GetRecentreCells();
}

// Everything the shader needs that is NOT the texture itself. Passed as one
// struct rather than as a growing argument list because the caller
// (UVoxelGISubsystem) diffs it against the last one it sent to decide whether a
// uniform-buffer rebuild is needed at all -- which is what makes
// voxel.GI.Volume's "read per frame" claim true instead of aspirational.
//
// Strength/AmbientFloor/Fade* are supplied by the caller rather than read from
// cvars here ON PURPOSE. The cvars live in VoxelEarth (voxel.GI.Strength,
// voxel.GI.AmbientFloor, voxel.GI.FadeStartUU, voxel.GI.FadeEndUU) and
// VoxelEarthShaders may not depend on VoxelEarth. Hardcoding them here -- which
// is what this file did until Wave B -- made the two shade formulas identical by
// coincidence (1.0 / 0.06) and silently divergent the moment either cvar moved.
struct FVoxelGIVolumeSettings
{
	FVector3f OriginPoolUU = FVector3f::ZeroVector;
	FVector3f FadeCentrePoolUU = FVector3f::ZeroVector;
	float Strength = 1.0f;
	float AmbientFloor = 0.06f;
	float FadeStartUU = 4800.0f;
	float FadeEndUU = 6400.0f;
	int32 DebugVis = 0;
	bool bEnabled = false;

	bool operator==(const FVoxelGIVolumeSettings& Other) const
	{
		return OriginPoolUU == Other.OriginPoolUU
			&& FadeCentrePoolUU == Other.FadeCentrePoolUU
			&& Strength == Other.Strength
			&& AmbientFloor == Other.AmbientFloor
			&& FadeStartUU == Other.FadeStartUU
			&& FadeEndUU == Other.FadeEndUU
			&& DebugVis == Other.DebugVis
			&& bEnabled == Other.bEnabled;
	}
	bool operator!=(const FVoxelGIVolumeSettings& Other) const { return !(*this == Other); }
};

// The volume itself. One per process, as a TGlobalResource -- the light field is
// centred on the camera and there is one camera.
class VOXELEARTHSHADERS_API FVoxelGIVolume : public FRenderResource
{
public:
	//~ FRenderResource
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;
	virtual FString GetFriendlyName() const override { return TEXT("FVoxelGIVolume"); }
	//~ End FRenderResource

	// Never returns null while GI is off: an unbound member of a uniform buffer
	// is a validation failure, not a tolerated no-op, so this hands back a
	// buffer pointing at GBlackVolumeTexture with Enabled=0.
	FRHIUniformBuffer* GetUniformBuffer() const { return UniformBuffer.GetReference(); }

	// Rebuilds the uniform buffer. Render thread. Cheap -- it is a handful of
	// scalars plus two resource pointers.
	void UpdateParameters_RenderThread(const FVoxelGIVolumeSettings& InSettings);

	// Creates the volume texture if it does not exist yet. Render thread.
	//
	// LAZY ON PURPOSE. The texture is N^3*4 bytes and this is a TGlobalResource,
	// so allocating it in InitRHI charges every session for it whether or not GI
	// is on -- 1 MB at the bring-up N=64, but 67 MB at the shipping N=256, which
	// is not a rounding error to hand to a player who never enables GI. Nothing
	// samples the volume while voxel.GI.Volume is 0 (the factory binds
	// GBlackVolumeTexture and Enabled=0), so there is nothing to allocate for.
	void EnsureAllocated_RenderThread(FRHICommandListBase& RHICmdList);

	// docs/gpu-gi-volume-design.md §7 step 1: fills every texel with a
	// per-brick checkerboard, A=255. This is the "does the same pool draw
	// known-good data?" rung -- a crisp 3.2 m checker aligned to chunk
	// boundaries proves creation, binding, PIXEL-shader reachability, the UVW
	// mapping, the origin and pool-space precision in ONE screenshot. Uniform
	// shading instead means the buffer reaches the VS but not the PS.
	void FillCheckerboard_RenderThread(FRHICommandListBase& RHICmdList);

	// Uploads one axis-aligned box of RGBA8 texels. SrcRGBA is tightly packed:
	// row pitch Size.X*4, depth pitch Size.X*Size.Y*4.
	//
	// RENDER THREAD ONLY -- BeginUpdateTexture3D_Internal asserts
	// IsInParallelRenderingThread(), and the SOURCE BYTES must already have been
	// copied out of the light field on the game thread under a read scope. The
	// one-shot UpdateTexture3D is used deliberately over the Begin/End pair:
	// EndUpdateTexture3D_Internal asserts the two land in the same render-thread
	// frame, and there is no reason to take on that hazard here.
	//
	// The caller is expected to have merged bricks into wide X-runs first. D3D12
	// stages at Align(Width*4, 256), so an 8-texel-wide brick moves 2 KB of
	// payload through 16 KB of staging plus a barrier; a 40-texel run of 5
	// bricks costs one call and one barrier for the same bytes.
	void UpdateTexels_RenderThread(FRHICommandListBase& RHICmdList,
	                               const FIntVector& DestMin, const FIntVector& Size,
	                               const uint8* SrcRGBA);

	int32 GetDimTexels() const { return DimTexels; }

private:
	FTextureRHIRef Volume;
	TUniformBufferRef<FVoxelGIVolumeParameters> UniformBuffer;
	FVoxelGIVolumeSettings Settings;
	int32 DimTexels = 0;
};

extern VOXELEARTHSHADERS_API TGlobalResource<FVoxelGIVolume> GVoxelGIVolume;
