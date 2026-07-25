#include "VoxelGIVolume.h"

#include "GlobalRenderResources.h"
#include "RenderUtils.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelGIVolumeParameters, "VoxelGIVol");

TGlobalResource<FVoxelGIVolume> GVoxelGIVolume;

namespace
{
	TAutoConsoleVariable<int32> CVarGIVolume(
		TEXT("voxel.GI.Volume"), 0,
		TEXT("Sample the voxel GI light field from a GPU volume texture instead of reading it off ")
		TEXT("baked vertex colours. 0 = off (default) and genuinely zero-cost: the vertex factory ")
		TEXT("skips the sample and emits byte-identical vertex colours. Read on the render thread ")
		TEXT("per frame, so it can be toggled live."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarGIVolumeDim(
		TEXT("voxel.GI.VolumeDim"), 64,
		TEXT("Per-axis texel count of the GI volume. Covers +/-(N*20) unreal units at 40 UU per cell, ")
		TEXT("and costs N^3 * 4 bytes: 64 -> 12.8 m, 1 MB; 256 -> 51.2 m, 67 MB. STARTUP ONLY -- the ")
		TEXT("texture is allocated once."),
		ECVF_ReadOnly);

	TAutoConsoleVariable<int32> CVarGIVolumeDebugVis(
		TEXT("voxel.GI.VolumeDebugVis"), 0,
		TEXT("0 = off. 3 = world checkerboard: fills the volume with a per-brick pattern instead of ")
		TEXT("irradiance. A crisp 3.2 m checker aligned to chunk boundaries proves the volume is ")
		TEXT("created, bound, reachable FROM THE PIXEL SHADER, and correctly addressed -- in one ")
		TEXT("screenshot. Uniform shading instead means the uniform buffer reaches the vertex shader ")
		TEXT("but not the pixel shader."),
		ECVF_RenderThreadSafe);
}

namespace VoxelGIVolume
{
	int32 GetDim()
	{
		// Clamped rather than trusted: this sizes an allocation, and N^3 grows
		// fast enough that a typo'd 1024 is 4 GB.
		return FMath::Clamp(CVarGIVolumeDim.GetValueOnAnyThread(), 16, 256);
	}

	bool IsEnabled()
	{
		return CVarGIVolume.GetValueOnAnyThread() != 0;
	}

	int32 GetDebugVis()
	{
		return CVarGIVolumeDebugVis.GetValueOnAnyThread();
	}
}

void FVoxelGIVolume::InitRHI(FRHICommandListBase& RHICmdList)
{
	DimTexels = VoxelGIVolume::GetDim();

	// NOT ETextureCreateFlags::Dynamic, and no lock/Map path anywhere. That is
	// the texture-side reading of gpu-pool-rendering-notes.md invariant 5, and
	// it is avoided by construction as long as every write goes through
	// UpdateTexture3D -- which, unlike the buffer lock path, does honour its
	// destination offsets (verified in D3D12Texture.cpp).
	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create3D(TEXT("VoxelGI.Irradiance"),
		                                DimTexels, DimTexels, DimTexels,
		                                PF_R8G8B8A8)
			.SetFlags(ETextureCreateFlags::ShaderResource);

	Volume = RHICmdList.CreateTexture(Desc);

	UE_LOG(LogTemp, Log,
	       TEXT("VoxelGI volume: dims=%dx%dx%d fmt=RGBA8 MB=%.1f coverage=+/-%.0f UU"),
	       DimTexels, DimTexels, DimTexels,
	       double(DimTexels) * DimTexels * DimTexels * 4.0 / (1024.0 * 1024.0),
	       0.5 * DimTexels * VoxelGIVolume::kCellSizeUU);

	UpdateParameters_RenderThread(OriginPoolUU);
}

void FVoxelGIVolume::ReleaseRHI()
{
	UniformBuffer.SafeRelease();
	Volume.SafeRelease();
	DimTexels = 0;
}

void FVoxelGIVolume::UpdateParameters_RenderThread(const FVector3f& InOriginPoolUU)
{
	OriginPoolUU = InOriginPoolUU;

	const bool bEnabled = VoxelGIVolume::IsEnabled() && Volume.IsValid();
	const float ExtentUU = FMath::Max(1.0f, float(DimTexels) * VoxelGIVolume::kCellSizeUU);

	FVoxelGIVolumeParameters Parameters;
	// Never null even when off -- an unbound member of a uniform buffer is a
	// validation failure, not a tolerated no-op (the factory's own buffer
	// documents the same rule for its SRVs).
	Parameters.Volume = bEnabled ? Volume.GetReference() : GBlackVolumeTexture->TextureRHI.GetReference();
	Parameters.VolumeSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters.OriginPoolUU = OriginPoolUU;
	Parameters.InvSizeUU = FVector3f(1.0f / ExtentUU);
	Parameters.Strength = 1.0f;
	Parameters.AmbientFloor = 0.06f;
	Parameters.FadeStartUU = 0.35f * ExtentUU;
	Parameters.FadeEndUU = 0.45f * ExtentUU;
	Parameters.Enabled = bEnabled ? 1u : 0u;
	Parameters.DebugVis = uint32(FMath::Max(0, VoxelGIVolume::GetDebugVis()));

	// MultiFrame, not SingleFrame: this is rebuilt when the volume re-centres or
	// a cvar moves, not every frame, so a single-frame lifetime would free it
	// out from under the next frame's draws.
	UniformBuffer = TUniformBufferRef<FVoxelGIVolumeParameters>::CreateUniformBufferImmediate(
		Parameters, UniformBuffer_MultiFrame);
}

void FVoxelGIVolume::FillCheckerboard_RenderThread(FRHICommandListBase& RHICmdList)
{
	if (!Volume.IsValid() || DimTexels <= 0)
	{
		return;
	}

	// One brick is 8 cells (320 UU), the same as one level-0 chunk, so the
	// checker lands exactly on chunk boundaries -- which is what makes a
	// misaligned volume origin obvious rather than merely suspicious.
	constexpr int32 kCellsPerBrick = 8;

	// Uploaded as whole-XY slabs, one Z at a time, NOT per brick. The D3D12
	// staging row pitch is Align(Width*4, 256), so an 8-texel-wide brick stages
	// 16 KB to move 2 KB -- 8x waste plus a barrier each. A full-width row has
	// zero waste. Same reasoning the real incremental path will need.
	TArray<uint8> Slab;
	Slab.SetNumUninitialized(DimTexels * DimTexels * 4);

	for (int32 Z = 0; Z < DimTexels; ++Z)
	{
		const int32 BrickZ = Z / kCellsPerBrick;
		for (int32 Y = 0; Y < DimTexels; ++Y)
		{
			const int32 BrickY = Y / kCellsPerBrick;
			for (int32 X = 0; X < DimTexels; ++X)
			{
				const int32 BrickX = X / kCellsPerBrick;
				const bool bLit = ((BrickX + BrickY + BrickZ) & 1) != 0;
				const uint8 V = bLit ? 255 : 96;
				uint8* Texel = Slab.GetData() + (int64(Y) * DimTexels + X) * 4;
				Texel[0] = V; Texel[1] = V; Texel[2] = V;
				Texel[3] = 255; // validity: every texel is "solved" in this pattern
			}
		}

		const FUpdateTextureRegion3D Region(0, 0, Z, 0, 0, 0, DimTexels, DimTexels, 1);
		RHICmdList.UpdateTexture3D(Volume, /*MipIndex*/ 0, Region,
		                           /*SourceRowPitch*/ uint32(DimTexels) * 4,
		                           /*SourceDepthPitch*/ uint32(DimTexels) * DimTexels * 4,
		                           Slab.GetData());
	}

	UE_LOG(LogTemp, Log,
	       TEXT("VoxelGI volume: checkerboard uploaded (%d slabs of %dx%d, %d bricks per axis)"),
	       DimTexels, DimTexels, DimTexels, DimTexels / kCellsPerBrick);
}

namespace
{
	// docs/gpu-gi-volume-design.md §7 step 1. Fills the volume with the
	// checkerboard and points it at the region around pool-space zero, which is
	// where the pool's rebase origin -- its first chunk -- sits, and therefore
	// where the camera is at spawn.
	//
	// A console command rather than a per-frame driver ON PURPOSE, for now. Step
	// 1 is bring-up: its whole job is to answer "is this path reachable from the
	// pixel shader at all", and a manual trigger keeps that answer independent of
	// any re-centring policy, which is step 4's problem. It also matches how
	// voxel.GPU.SpawnPool is driven.
	void GIVolumeTestCommand(const TArray<FString>& Args)
	{
		const int32 Dim = VoxelGIVolume::GetDim();
		const float ExtentUU = float(Dim) * VoxelGIVolume::kCellSizeUU;
		// Centre the volume on pool-space zero.
		const FVector3f OriginPoolUU(-0.5f * ExtentUU);

		UE_LOG(LogTemp, Log,
		       TEXT("VoxelGI volume: TEST requested (enabled=%d debugVis=%d dim=%d originPool=(%.0f,%.0f,%.0f))"),
		       VoxelGIVolume::IsEnabled() ? 1 : 0, VoxelGIVolume::GetDebugVis(), Dim,
		       OriginPoolUU.X, OriginPoolUU.Y, OriginPoolUU.Z);

		ENQUEUE_RENDER_COMMAND(VoxelGIVolumeTest)(
			[OriginPoolUU](FRHICommandListImmediate& RHICmdList)
		{
			GVoxelGIVolume.FillCheckerboard_RenderThread(RHICmdList);
			GVoxelGIVolume.UpdateParameters_RenderThread(OriginPoolUU);
		});
	}

	FAutoConsoleCommand GVoxelGIVolumeTestCmd(
		TEXT("voxel.GI.VolumeTest"),
		TEXT("Fill the GI volume with a per-brick checkerboard and bind it. Bring-up diagnostic: a crisp ")
		TEXT("3.2 m checker aligned to chunk boundaries proves the volume is created, bound, reachable from ")
		TEXT("the PIXEL shader, and correctly addressed. Needs voxel.GI.Volume 1."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&GIVolumeTestCommand));
}
