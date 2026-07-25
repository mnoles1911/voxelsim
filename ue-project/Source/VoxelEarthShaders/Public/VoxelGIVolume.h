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
	SHADER_PARAMETER(float, Strength)
	SHADER_PARAMETER(float, AmbientFloor)
	SHADER_PARAMETER(float, FadeStartUU)
	SHADER_PARAMETER(float, FadeEndUU)
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
}

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
	void UpdateParameters_RenderThread(const FVector3f& InOriginPoolUU);

	// docs/gpu-gi-volume-design.md §7 step 1: fills every texel with a
	// per-brick checkerboard, A=255. This is the "does the same pool draw
	// known-good data?" rung -- a crisp 3.2 m checker aligned to chunk
	// boundaries proves creation, binding, PIXEL-shader reachability, the UVW
	// mapping, the origin and pool-space precision in ONE screenshot. Uniform
	// shading instead means the buffer reaches the VS but not the PS.
	void FillCheckerboard_RenderThread(FRHICommandListBase& RHICmdList);

	int32 GetDimTexels() const { return DimTexels; }

private:
	FTextureRHIRef Volume;
	TUniformBufferRef<FVoxelGIVolumeParameters> UniformBuffer;
	FVector3f OriginPoolUU = FVector3f::ZeroVector;
	int32 DimTexels = 0;
};

extern VOXELEARTHSHADERS_API TGlobalResource<FVoxelGIVolume> GVoxelGIVolume;
