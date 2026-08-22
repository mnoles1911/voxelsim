#include "VoxelGIVolume.h"

#include "GlobalRenderResources.h"
// RegisterVolumesForCompute only, for the free RegisterExternalTexture() -- the
// same three-argument form VoxelShadowMarch.cpp already uses, which lives in
// RenderGraphUtils.h rather than RenderGraphBuilder.h. The header deliberately
// carries only RenderGraphFwd, so the graph machinery stops here.
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelGIVolumeParameters, "VoxelGIVol");

TGlobalResource<FVoxelGIVolume> GVoxelGIVolume;

namespace
{
	// DEFAULT IS 1, AND THE HELP TEXT USED TO SAY 0.
	//
	// The exact defect VoxelGI.cpp's CVarGIEnabled carries a note about, in the
	// second of the two cvars that decide whether GI runs -- and this one was
	// still uncorrected on 2026-08-20, after the other had been fixed and after
	// the correction had already been written into
	// docs/gpu-roadmap-remaining.md and docs/ray-marching-plan-2026-08-19.md. It
	// read "0 = off (default)" on the line BELOW a literal 1. A cvar whose
	// description contradicts its own default one line up is worse than an
	// undocumented one, because it is believed: a Phase 7 brief was written on
	// the premise that voxel GI ships off, and it was wrong about both switches.
	//
	// WHAT IT COST: every leg on this project has run with the volume ON, so its
	// cost is already inside the 18.99 ms A0 baseline and the 34.7 ms shadowed
	// one rather than waiting outside them. Anyone budgeting a GI feature
	// against those numbers while believing this is 0 double-counts it.
	//
	// The "genuinely zero-cost when 0" claim IS still true and is kept, because
	// it is the property that makes voxel.GI.Volume 0 a valid control arm.
	TAutoConsoleVariable<int32> CVarGIVolume(
		TEXT("voxel.GI.Volume"), 1,   // 2026-07-27: ON for the manual PIE evaluation.
		TEXT("Sample the voxel GI light field from a GPU volume texture instead of reading it off ")
		TEXT("baked vertex colours. DEFAULT 1 (on) since 2026-07-27. 0 = off, and genuinely ")
		TEXT("zero-cost: the vertex factory skips the sample, the texture is not even allocated, ")
		TEXT("and the emitted vertex colours are byte-identical. Toggling it live works: ")
		TEXT("UVoxelGISubsystem re-pushes the uniform buffer from its tick whenever any input to it ")
		TEXT("changes. NOTE it needs a CONSUMER -- only the pooled vertex factory samples the ")
		TEXT("volume, so with voxel.Stream.GPU 0 this allocates nothing and changes nothing."),
		ECVF_RenderThreadSafe);

	// docs/gpu-gi-volume-design.md §4. Half-width of the box the camera may move
	// inside before the volume re-centres, in 40 UU cells. 64 -> +/-2560 UU.
	TAutoConsoleVariable<int32> CVarGIVolumeRecentreCells(
		TEXT("voxel.GI.VolumeRecentreCells"), 64,
		TEXT("Dead-zone half-width for camera-following re-centring of the GI volume, in 40 UU cells. ")
		TEXT("The volume re-centres when the camera leaves this box around the volume centre, staging ")
		TEXT("the whole texture's worth of re-addressed texels over a few frames and swapping the ")
		TEXT("origin uniform on exactly the frame the last upload lands. Smaller = the volume tracks ")
		TEXT("the camera more tightly and re-centres more often (each re-centre is Dim^3*4 bytes of ")
		TEXT("upload); larger = fewer re-centres but the camera sits further off centre. Clamped to ")
		TEXT("leave the camera inside the volume."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIVolumeDim(
		TEXT("voxel.GI.VolumeDim"), 192,  // 2026-07-27: 64 covers only +/-12.8 m (~36 of ~1,950 resident bricks) -- not enough to judge by eye. 192 is the size Wave B measured Scheme A at.
		TEXT("Per-axis texel count of the GI volume. Covers +/-(N*20) unreal units at 40 UU per cell, ")
		TEXT("and costs N^3 * 4 bytes: 64 -> 12.8 m, 1 MB; 256 -> 51.2 m, 67 MB. STARTUP ONLY -- the ")
		TEXT("texture is allocated once."),
		ECVF_ReadOnly);

	// P7 PREREQUISITE, SHIPPED OFF. Makes VolumePos/VolumeNeg writable by a
	// compute pass.
	//
	// WHY IT IS NEEDED AT ALL. Every write to these two textures today goes
	// through RHICmdList.UpdateTexture3D from the CPU, so they are created with
	// ETextureCreateFlags::ShaderResource and nothing else. A GPU cone march
	// that produces irradiance directly from the resident brick pool has to
	// write them from a UAV, and a texture's flags are fixed at creation -- so
	// this cannot be added later by the pass that needs it, only by the
	// allocation that runs once at startup. Landing the flag now decouples that
	// prerequisite from the march itself, which is blocked on something else
	// entirely (see the note on VOXEL_MARCH_LEVEL_COUNT in the report
	// accompanying this change).
	//
	// WHY IT DEFAULTS TO 0, AND THIS IS THE SAME RULE voxel.GI.Volume AND T4-1
	// SHIPPED UNDER: the first build with a new capability compiled in must not
	// be able to regress anything. At 0 the create descriptors are BYTE-
	// IDENTICAL to what they were before this cvar existed, no UAV is created,
	// and nothing downstream can observe the difference. At 1 the two textures
	// gain ETextureCreateFlags::UAV and nothing else changes -- there is still
	// no compute writer, so 1 is a VRAM-and-flags arm, not a feature.
	//
	// A UAV FLAG IS NOT FREE ON EVERY DRIVER, WHICH IS THE OTHER REASON FOR THE
	// SWITCH. Declaring a resource UAV-capable can cost it compression and
	// fast-clear paths, and on a 192^3 RGBA8 pair that is 56.6 MB of texture
	// whose sampling cost is on the critical path of every terrain pixel. That
	// is a measurable claim and it is NOT measured here; the switch is what
	// makes it measurable as a two-arm A/B at some later point, on the whole
	// frame, rather than argued.
	//
	// STARTUP ONLY, like VolumeDim above and for exactly the same reason: it is
	// read inside EnsureAllocated_RenderThread, which runs once. An -ExecCmds
	// toggle lands after allocation and changes nothing, silently -- so it is
	// ECVF_ReadOnly to make that a refusal rather than a mystery.
	TAutoConsoleVariable<int32> CVarGIVolumeUAV(
		TEXT("voxel.GI.VolumeUAV"), 0,
		TEXT("P7 prerequisite. 1 = allocate VolumePos/VolumeNeg with ETextureCreateFlags::UAV so a ")
		TEXT("compute pass can write them; 0 (default) = byte-identical create descriptors to before ")
		TEXT("this existed. There is NO compute writer yet, so 1 changes only the flags and whatever ")
		TEXT("the driver does with them -- it is not a feature switch. STARTUP ONLY: the textures are ")
		TEXT("allocated once, so setting this after allocation does nothing. Check the 'uav=' field ")
		TEXT("on the 'VoxelGI volume: allocated' line to see which arm a run actually got."),
		ECVF_ReadOnly);

	// docs/sky-and-local-light-plan.md §2.2 / phase L1. SHIPPED OFF, exactly as
	// voxel.GI.Volume and T4-1 were: the first build with this compiled in must
	// not be able to regress anything, and with this at 0 the factory does not
	// sample VolumeLocal, the texture is not allocated, no CPU mirror is
	// allocated, and the emitted vertex colour is byte-identical.
	TAutoConsoleVariable<int32> CVarGILocalLights(
		TEXT("voxel.GI.LocalLights"), 0,
		TEXT("Local light injection (L1: the UN-CRUSH half). 0 = off (default) and genuinely zero-cost. ")
		TEXT("1 = registered local lights are splatted into VolumeLocal's A channel and the pooled vertex ")
		TEXT("factory takes Ambient = max(Ambient, A) before folding it into vertex colour G. THIS IS NOT ")
		TEXT("EXTRA LIGHT: it is what gives a deferred point light a real BaseColor to work against, since ")
		TEXT("in a cave G ~= AO * lerp(0.06, 1, ~0) ~= 0.06 and a torch would otherwise light a wall at 6%% ")
		TEXT("of its albedo. The term stays a RELATIVE modulation <= 1 with no time-of-day input, so the ")
		TEXT("composition contract in docs/lighting-weather-plan.md 2.3 holds. Needs voxel.GI.Volume 1 and ")
		TEXT("a CONSUMER (only the pooled vertex factory samples the volume)."),
		ECVF_RenderThreadSafe);

	// READ ONLY, like voxel.GI.VolumeDim and for the same reason: it sizes an
	// allocation made once, and both the render thread (the texture) and the game
	// thread (the CPU mirror) read it independently -- a value that could move
	// between those two reads would mis-size one of them silently.
	TAutoConsoleVariable<int32> CVarGILocalVolumeDim(
		TEXT("voxel.GI.LocalVolumeDim"), 96,
		TEXT("Per-axis texel count of VolumeLocal. It covers the SAME BOX as the irradiance volumes (same ")
		TEXT("origin, same UVW, zero new interpolants), so its texel SIZE is voxel.GI.VolumeDim*40 / this: ")
		TEXT("96 against the default VolumeDim 192 is an 80 UU texel, 3.4 MB of VRAM plus an equal CPU ")
		TEXT("mirror. Costs N^3*4 bytes. Escalate to 192 only if the wall-leak gate fails -- that is L2's ")
		TEXT("decision, not a knob to turn first. STARTUP ONLY."),
		ECVF_ReadOnly);

	TAutoConsoleVariable<int32> CVarGIVolumeDebugVis(
		TEXT("voxel.GI.VolumeDebugVis"), 0,
		TEXT("0 = off. 3 = world checkerboard: fills the volume with a per-brick pattern instead of ")
		TEXT("irradiance. A crisp 3.2 m checker aligned to chunk boundaries proves the volume is ")
		TEXT("created, bound, reachable FROM THE PIXEL SHADER, and correctly addressed -- in one ")
		TEXT("screenshot. Uniform shading instead means the uniform buffer reaches the vertex shader ")
		TEXT("but not the pixel shader. 4 = the same rung for VolumeLocal: shows its A channel RAW, ")
		TEXT("ignoring AO, the ambient floor and the fade, so a checker (voxel.GI.VolumeLocalTest) or a ")
		TEXT("torch's falloff sphere is legible without also being a statement about the un-crush maths."),
		ECVF_RenderThreadSafe);
}

namespace VoxelGIVolume
{
	int32 GetDim()
	{
		// Clamped rather than trusted: this sizes an allocation, and N^3 grows
		// fast enough that a typo'd 1024 is 4 GB.
		//
		// Rounded DOWN to a multiple of 8 as well, so the volume is a whole
		// number of light-field bricks on every axis. Otherwise the brick at the
		// far face is partially outside the texture and every upload path needs
		// a clip case that would only ever execute for someone who typed 100.
		const int32 Clamped = FMath::Clamp(CVarGIVolumeDim.GetValueOnAnyThread(), 16, 256);
		return Clamped & ~7;
	}

	int32 GetLocalDim()
	{
		// Same shape as GetDim() above, deliberately -- clamped because it sizes
		// an allocation, and rounded DOWN to a multiple of 8 so the volume is a
		// whole number of light-field bricks on every axis and no upload path
		// needs a partial-brick clip case.
		const int32 Clamped = FMath::Clamp(CVarGILocalVolumeDim.GetValueOnAnyThread(), 16, 256);
		return Clamped & ~7;
	}

	bool IsEnabled()
	{
		return CVarGIVolume.GetValueOnAnyThread() != 0;
	}

	bool IsUAVEnabled()
	{
		return CVarGIVolumeUAV.GetValueOnAnyThread() != 0;
	}

	bool IsLocalEnabled()
	{
		return CVarGILocalLights.GetValueOnAnyThread() != 0;
	}

	int32 GetDebugVis()
	{
		return CVarGIVolumeDebugVis.GetValueOnAnyThread();
	}

	int32 GetRecentreCells()
	{
		return FMath::Max(1, CVarGIVolumeRecentreCells.GetValueOnAnyThread());
	}
}

void FVoxelGIVolume::InitRHI(FRHICommandListBase& RHICmdList)
{
	DimTexels = VoxelGIVolume::GetDim();
	// The TEXTURE is deliberately not created here -- see
	// EnsureAllocated_RenderThread. What is created is the uniform buffer, which
	// must exist and must not contain a null member from the very first draw.
	UpdateParameters_RenderThread(Settings);
}

void FVoxelGIVolume::EnsureAllocated_RenderThread(FRHICommandListBase& RHICmdList, bool bWantLocal)
{
	// TWO INDEPENDENT LAZY ALLOCATIONS, not one early return.
	//
	// This used to be `if (VolumePos.IsValid()) return;`, which would have made
	// VolumeLocal permanently unallocatable the moment the irradiance volumes
	// existed -- i.e. always, since voxel.GI.Volume defaults on and
	// voxel.GI.LocalLights defaults off. Each texture therefore guards itself, and
	// the caller re-enters this function when voxel.GI.LocalLights turns on.
	bool bCreatedAny = false;

	if (!VolumePos.IsValid())
	{
		if (DimTexels <= 0)
		{
			DimTexels = VoxelGIVolume::GetDim();
		}

		// NOT ETextureCreateFlags::Dynamic, and no lock/Map path anywhere. That is
		// the texture-side reading of gpu-pool-rendering-notes.md invariant 5, and
		// it is avoided by construction as long as every write goes through
		// UpdateTexture3D -- which, unlike the buffer lock path, does honour its
		// destination offsets (verified in D3D12Texture.cpp).
		// P7 PREREQUISITE. voxel.GI.VolumeUAV adds ETextureCreateFlags::UAV so a
		// compute pass can write these; at its default 0 this expression is
		// exactly ShaderResource and the descriptors are byte-identical to what
		// they were before the cvar existed.
		//
		// LATCHED INTO bAllocatedWithUAV, not re-read later. A texture's flags
		// are fixed at creation and this function runs ONCE, so a later read of
		// the cvar would describe what was asked for rather than what exists --
		// which is the shape of defect this file already carries two notes
		// about. Anything that wants to know whether a UAV is available must
		// ask WasAllocatedWithUAV(), never the cvar.
		bAllocatedWithUAV = VoxelGIVolume::IsUAVEnabled();
		const ETextureCreateFlags VolumeFlags =
			ETextureCreateFlags::ShaderResource
			| (bAllocatedWithUAV ? ETextureCreateFlags::UAV : ETextureCreateFlags::None);

		const FRHITextureCreateDesc DescPos =
			FRHITextureCreateDesc::Create3D(TEXT("VoxelGI.IrradiancePos"),
			                                DimTexels, DimTexels, DimTexels,
			                                PF_R8G8B8A8)
				.SetFlags(VolumeFlags);
		const FRHITextureCreateDesc DescNeg =
			FRHITextureCreateDesc::Create3D(TEXT("VoxelGI.IrradianceNeg"),
			                                DimTexels, DimTexels, DimTexels,
			                                PF_R8G8B8A8)
				.SetFlags(VolumeFlags);

		VolumePos = RHICmdList.CreateTexture(DescPos);
		VolumeNeg = RHICmdList.CreateTexture(DescNeg);

		// uav= REPORTS WHAT WAS ALLOCATED, NOT WHAT WAS REQUESTED. These flags
		// are unchangeable after this line, the allocation happens once, and
		// voxel.GI.VolumeUAV is ECVF_ReadOnly precisely because a late set is a
		// silent no-op -- so the log has to carry the achieved state or a run's
		// arm is unknowable from its own output.
		//
		// GREP: "VoxelGI volume: allocated"
		UE_LOG(LogTemp, Log,
		       TEXT("VoxelGI volume: allocated 2x dims=%dx%dx%d fmt=RGBA8 (Scheme A) MB=%.1f ")
		       TEXT("coverage=+/-%.0f UU uav=%d (%s)"),
		       DimTexels, DimTexels, DimTexels,
		       2.0 * double(DimTexels) * DimTexels * DimTexels * 4.0 / (1024.0 * 1024.0),
		       0.5 * DimTexels * VoxelGIVolume::kCellSizeUU,
		       bAllocatedWithUAV ? 1 : 0,
		       bAllocatedWithUAV
		           ? TEXT("UAV-capable: a compute pass MAY write these. No writer exists yet, so this ")
		             TEXT("arm differs from the default only in flags")
		           : TEXT("sampled-only, byte-identical to pre-P7 -- RegisterVolumesForCompute will refuse"));

		// A freshly created texture's contents are undefined, and undefined bytes in
		// the validity channel read as "there is data here" -- i.e. arbitrary
		// irradiance on every surface until the first upload covers that texel.
		// Zeroing means A=0 everywhere, which the shader turns into plain AO.
		{
			TArray<uint8> ZeroSlab;
			ZeroSlab.SetNumZeroed(int64(DimTexels) * DimTexels * 4);
			for (int32 Z = 0; Z < DimTexels; ++Z)
			{
				const FUpdateTextureRegion3D Slab(0, 0, Z, 0, 0, 0, DimTexels, DimTexels, 1);
				RHICmdList.UpdateTexture3D(VolumePos, 0, Slab,
				                           uint32(DimTexels) * 4, uint32(DimTexels) * DimTexels * 4,
				                           ZeroSlab.GetData());
				RHICmdList.UpdateTexture3D(VolumeNeg, 0, Slab,
				                           uint32(DimTexels) * 4, uint32(DimTexels) * DimTexels * 4,
				                           ZeroSlab.GetData());
			}
		}
		bCreatedAny = true;
	}

	// VolumeLocal, gated on the CALLER'S bWantLocal as well as on being unallocated.
	// Lazy because this is a TGlobalResource: allocating it unconditionally would
	// charge 3.4 MB to every session including the ones that never turn local
	// lights on, and nothing samples it while voxel.GI.LocalLights is 0 (the
	// factory binds GBlackVolumeTexture and LocalEnabled=0).
	//
	// PASSED IN, NEVER `VoxelGIVolume::IsLocalEnabled()` READ HERE, and that is a
	// bug fix rather than a preference. voxel.GI.LocalLights is
	// ECVF_RenderThreadSafe, so GetValueOnAnyThread() returns the RENDER-THREAD
	// SHADOW value on this thread -- and that shadow is only copied from the game
	// value by the once-per-frame console variable sink. `voxel.GI.LocalLights 1`
	// therefore reads 1 on the game thread that enqueues this command and can still
	// read 0 on the render thread that executes it. The caller latches
	// bLocalVolumeAllocated=true regardless (VoxelGI.h), so losing that race meant
	// the texture was never created, never retried, LocalEnabled bound 0 forever and
	// LastVolumeSettings.bLocalEnabled latched 1 -- docs/sky-and-local-light-plan.md
	// §5 item 5 exactly, reintroduced one level down from the operator== it warns
	// about. The irradiance volumes above never had this exposure because they are
	// gated on nothing but !IsValid(). Intent now travels with the command.
	if (!VolumeLocal.IsValid() && bWantLocal)
	{
		if (LocalDimTexels <= 0)
		{
			LocalDimTexels = VoxelGIVolume::GetLocalDim();
		}
		const FRHITextureCreateDesc DescLocal =
			FRHITextureCreateDesc::Create3D(TEXT("VoxelGI.Local"),
			                                LocalDimTexels, LocalDimTexels, LocalDimTexels,
			                                PF_R8G8B8A8)
				.SetFlags(ETextureCreateFlags::ShaderResource);
		VolumeLocal = RHICmdList.CreateTexture(DescLocal);

		const float LocalTexelUU = float(DimTexels > 0 ? DimTexels : VoxelGIVolume::GetDim())
		                         * VoxelGIVolume::kCellSizeUU / float(FMath::Max(1, LocalDimTexels));
		UE_LOG(LogTemp, Log,
		       TEXT("VoxelGI volume: allocated VolumeLocal dims=%dx%dx%d fmt=RGBA8 MB=%.1f texel=%.0f UU ")
		       TEXT("(voxel.GI.LocalVolumeDim=%d, same box as the irradiance volumes)"),
		       LocalDimTexels, LocalDimTexels, LocalDimTexels,
		       double(LocalDimTexels) * LocalDimTexels * LocalDimTexels * 4.0 / (1024.0 * 1024.0),
		       LocalTexelUU, LocalDimTexels);

		// ZEROED EXPLICITLY, for the same reason the volumes above are: a fresh
		// texture's contents are undefined, and an undefined A here reads as "this
		// cell is fully lit by a local source", i.e. every surface in the volume
		// un-crushed to full albedo until the first splat covers that texel. Note
		// the slab is sized from LocalDimTexels, NOT from the DimTexels the loop
		// above uses -- the two volumes have independent dims and reusing the
		// sibling's Dim*Dim*4 slab would either under-fill or overrun.
		{
			TArray<uint8> ZeroSlab;
			ZeroSlab.SetNumZeroed(int64(LocalDimTexels) * LocalDimTexels * 4);
			for (int32 Z = 0; Z < LocalDimTexels; ++Z)
			{
				const FUpdateTextureRegion3D Slab(0, 0, Z, 0, 0, 0, LocalDimTexels, LocalDimTexels, 1);
				RHICmdList.UpdateTexture3D(VolumeLocal, 0, Slab,
				                           uint32(LocalDimTexels) * 4,
				                           uint32(LocalDimTexels) * LocalDimTexels * 4,
				                           ZeroSlab.GetData());
			}
		}
		bCreatedAny = true;
	}

	if (bCreatedAny)
	{
		// Re-publish so Parameters.Volume* stop pointing at GBlackVolumeTexture.
		UpdateParameters_RenderThread(Settings);
	}
}

void FVoxelGIVolume::ReleaseRHI()
{
	UniformBuffer.SafeRelease();
	VolumePos.SafeRelease();
	VolumeNeg.SafeRelease();
	VolumeLocal.SafeRelease();
	DimTexels = 0;
	LocalDimTexels = 0;
}

void FVoxelGIVolume::UpdateParameters_RenderThread(const FVoxelGIVolumeSettings& InSettings)
{
	Settings = InSettings;

	const bool bEnabled = Settings.bEnabled && VolumePos.IsValid() && VolumeNeg.IsValid();
	// AND'ed with bEnabled, not independent of it: the local sample lives inside
	// the factory's `if (Enabled != 0u && bInsideVolume)` block, so LocalEnabled=1
	// with Enabled=0 would be a claim the shader can never act on.
	const bool bLocalEnabled = bEnabled && Settings.bLocalEnabled && VolumeLocal.IsValid();
	const float ExtentUU = FMath::Max(1.0f, float(DimTexels) * VoxelGIVolume::kCellSizeUU);

	FVoxelGIVolumeParameters Parameters;
	// Never null even when off -- an unbound member of a uniform buffer is a
	// validation failure, not a tolerated no-op (the factory's own buffer
	// documents the same rule for its SRVs).
	Parameters.VolumePos = bEnabled ? VolumePos.GetReference() : GBlackVolumeTexture->TextureRHI.GetReference();
	Parameters.VolumeNeg = bEnabled ? VolumeNeg.GetReference() : GBlackVolumeTexture->TextureRHI.GetReference();
	// NO MEMBER MAY BE NULL, including this one when local lights are off. Black
	// is also the correct VALUE to fall back to: A=0 means "no local source here",
	// which the factory's max(Ambient, L.a) turns into no change at all.
	Parameters.VolumeLocal = bLocalEnabled ? VolumeLocal.GetReference() : GBlackVolumeTexture->TextureRHI.GetReference();
	Parameters.VolumeSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters.OriginPoolUU = Settings.OriginPoolUU;
	Parameters.OriginRelCameraUU = Settings.OriginRelCameraUU;
	Parameters.InvSizeUU = FVector3f(1.0f / ExtentUU);
	Parameters.FadeCentrePoolUU = Settings.FadeCentrePoolUU;
	// SUPPLIED, not invented here. Until Wave B these four were hardcoded to
	// 1.0 / 0.06 and to fractions of the volume extent, which (a) silently
	// ignored voxel.GI.Strength and voxel.GI.AmbientFloor and (b) made the fade
	// radii shrink with voxel.GI.VolumeDim while the CPU path kept using
	// 4800/6400 -- the design doc's risk 8, live, as a plausible-looking
	// lighting ring at the volume face.
	Parameters.Strength = Settings.Strength;
	Parameters.AmbientFloor = Settings.AmbientFloor;
	Parameters.FadeStartUU = Settings.FadeStartUU;
	Parameters.FadeEndUU = Settings.FadeEndUU;
	Parameters.CellSizeUU = VoxelGIVolume::kCellSizeUU;
	Parameters.Enabled = bEnabled ? 1u : 0u;
	Parameters.LocalEnabled = bLocalEnabled ? 1u : 0u;
	Parameters.DebugVis = uint32(FMath::Max(0, Settings.DebugVis));

	// MultiFrame, not SingleFrame: this is rebuilt when the volume re-centres or
	// a cvar moves, not every frame, so a single-frame lifetime would free it
	// out from under the next frame's draws.
	UniformBuffer = TUniformBufferRef<FVoxelGIVolumeParameters>::CreateUniformBufferImmediate(
		Parameters, UniformBuffer_MultiFrame);
}

void FVoxelGIVolume::FillCheckerboard_RenderThread(FRHICommandListBase& RHICmdList)
{
	if (!VolumePos.IsValid() || DimTexels <= 0)
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
		RHICmdList.UpdateTexture3D(VolumePos, /*MipIndex*/ 0, Region,
		                           /*SourceRowPitch*/ uint32(DimTexels) * 4,
		                           /*SourceDepthPitch*/ uint32(DimTexels) * DimTexels * 4,
		                           Slab.GetData());
		RHICmdList.UpdateTexture3D(VolumeNeg, /*MipIndex*/ 0, Region,
		                           /*SourceRowPitch*/ uint32(DimTexels) * 4,
		                           /*SourceDepthPitch*/ uint32(DimTexels) * DimTexels * 4,
		                           Slab.GetData());
	}

	UE_LOG(LogTemp, Log,
	       TEXT("VoxelGI volume: checkerboard uploaded (%d slabs of %dx%d, %d bricks per axis)"),
	       DimTexels, DimTexels, DimTexels, DimTexels / kCellsPerBrick);
}

void FVoxelGIVolume::FillLocalCheckerboard_RenderThread(FRHICommandListBase& RHICmdList)
{
	if (!VolumeLocal.IsValid() || LocalDimTexels <= 0)
	{
		UE_LOG(LogTemp, Warning,
		       TEXT("VoxelGI volume: local checkerboard skipped -- VolumeLocal is not allocated ")
		       TEXT("(needs voxel.GI.Volume 1 AND voxel.GI.LocalLights 1, and one tick of the GI subsystem ")
		       TEXT("to allocate it)."));
		return;
	}

	// The brick period is expressed in LOCAL texels, computed from the ratio of
	// the two dims rather than hardcoded to 8: the two volumes cover the same box
	// with different resolutions, so one light-field brick is
	// LocalDim/(Dim/8) local texels -- 4 of them at the default 96 against 192.
	// Hardcoding 8 here would draw a checker of the wrong period and then invite
	// somebody to "fix" the origin snap that is not broken.
	const int32 MainDim = DimTexels > 0 ? DimTexels : VoxelGIVolume::GetDim();
	const int32 TexelsPerBrick = FMath::Max(1, (LocalDimTexels * 8) / FMath::Max(1, MainDim));

	// A ONLY, RGB left at zero. That is L1's channel contract stated in the
	// instrument as well as in the shader: if this rung ever paints RGB, a later
	// reader cannot tell an L1 build from a half-landed L2 one.
	TArray<uint8> Slab;
	Slab.SetNumUninitialized(int64(LocalDimTexels) * LocalDimTexels * 4);

	for (int32 Z = 0; Z < LocalDimTexels; ++Z)
	{
		const int32 BrickZ = Z / TexelsPerBrick;
		for (int32 Y = 0; Y < LocalDimTexels; ++Y)
		{
			const int32 BrickY = Y / TexelsPerBrick;
			for (int32 X = 0; X < LocalDimTexels; ++X)
			{
				const int32 BrickX = X / TexelsPerBrick;
				const bool bLit = ((BrickX + BrickY + BrickZ) & 1) != 0;
				uint8* Texel = Slab.GetData() + (int64(Y) * LocalDimTexels + X) * 4;
				Texel[0] = 0; Texel[1] = 0; Texel[2] = 0;  // RGB is L2's, not L1's
				Texel[3] = bLit ? 255 : 0;                 // the un-crush scalar
			}
		}

		const FUpdateTextureRegion3D Region(0, 0, Z, 0, 0, 0, LocalDimTexels, LocalDimTexels, 1);
		RHICmdList.UpdateTexture3D(VolumeLocal, /*MipIndex*/ 0, Region,
		                           /*SourceRowPitch*/ uint32(LocalDimTexels) * 4,
		                           /*SourceDepthPitch*/ uint32(LocalDimTexels) * LocalDimTexels * 4,
		                           Slab.GetData());
	}

	UE_LOG(LogTemp, Log,
	       TEXT("VoxelGI volume: LOCAL checkerboard uploaded (%d slabs of %dx%d, period %d texels = one ")
	       TEXT("light-field brick). Read it IN A CAVE with voxel.GI.VolumeDebugVis 4 -- where Ambient is ")
	       TEXT("already 1 the un-crush is a no-op by design."),
	       LocalDimTexels, LocalDimTexels, LocalDimTexels, TexelsPerBrick);
}

void FVoxelGIVolume::UpdateTexels_RenderThread(FRHICommandListBase& RHICmdList,
                                               const FIntVector& DestMin, const FIntVector& Size,
                                               const uint8* SrcPos, const uint8* SrcNeg)
{
	if (!VolumePos.IsValid() || !VolumeNeg.IsValid() || DimTexels <= 0 || !SrcPos || !SrcNeg)
	{
		return;
	}
	if (Size.X <= 0 || Size.Y <= 0 || Size.Z <= 0)
	{
		return;
	}
	// Silently clipping would hide an addressing bug behind a plausible image,
	// which is the failure mode this whole module is organised around. The
	// driver never produces an out-of-range box, so this is an assert with a
	// log rather than a fixup.
	if (DestMin.X < 0 || DestMin.Y < 0 || DestMin.Z < 0 ||
	    DestMin.X + Size.X > DimTexels || DestMin.Y + Size.Y > DimTexels || DestMin.Z + Size.Z > DimTexels)
	{
		UE_LOG(LogTemp, Warning,
		       TEXT("VoxelGI volume: rejected out-of-range upload min=(%d,%d,%d) size=(%d,%d,%d) dim=%d"),
		       DestMin.X, DestMin.Y, DestMin.Z, Size.X, Size.Y, Size.Z, DimTexels);
		return;
	}

	const FUpdateTextureRegion3D Region(uint32(DestMin.X), uint32(DestMin.Y), uint32(DestMin.Z),
	                                    0, 0, 0,
	                                    uint32(Size.X), uint32(Size.Y), uint32(Size.Z));
	RHICmdList.UpdateTexture3D(VolumePos, /*MipIndex*/ 0, Region,
	                           /*SourceRowPitch*/ uint32(Size.X) * 4,
	                           /*SourceDepthPitch*/ uint32(Size.X) * uint32(Size.Y) * 4,
	                           SrcPos);
	RHICmdList.UpdateTexture3D(VolumeNeg, /*MipIndex*/ 0, Region,
	                           /*SourceRowPitch*/ uint32(Size.X) * 4,
	                           /*SourceDepthPitch*/ uint32(Size.X) * uint32(Size.Y) * 4,
	                           SrcNeg);
}

void FVoxelGIVolume::UpdateLocalTexels_RenderThread(FRHICommandListBase& RHICmdList,
                                                    const FIntVector& DestMin, const FIntVector& Size,
                                                    const uint8* SrcLocal)
{
	if (!VolumeLocal.IsValid() || LocalDimTexels <= 0 || !SrcLocal)
	{
		return;
	}
	if (Size.X <= 0 || Size.Y <= 0 || Size.Z <= 0)
	{
		return;
	}
	// Refused with a log rather than clipped, for the same reason the sibling
	// above refuses: silently clipping hides an addressing bug behind a plausible
	// image, and the extent this is checked against is LocalDimTexels -- the whole
	// point of this being a separate function.
	if (DestMin.X < 0 || DestMin.Y < 0 || DestMin.Z < 0 ||
	    DestMin.X + Size.X > LocalDimTexels || DestMin.Y + Size.Y > LocalDimTexels ||
	    DestMin.Z + Size.Z > LocalDimTexels)
	{
		UE_LOG(LogTemp, Warning,
		       TEXT("VoxelGI volume: rejected out-of-range LOCAL upload min=(%d,%d,%d) size=(%d,%d,%d) localDim=%d"),
		       DestMin.X, DestMin.Y, DestMin.Z, Size.X, Size.Y, Size.Z, LocalDimTexels);
		return;
	}

	const FUpdateTextureRegion3D Region(uint32(DestMin.X), uint32(DestMin.Y), uint32(DestMin.Z),
	                                    0, 0, 0,
	                                    uint32(Size.X), uint32(Size.Y), uint32(Size.Z));
	RHICmdList.UpdateTexture3D(VolumeLocal, /*MipIndex*/ 0, Region,
	                           /*SourceRowPitch*/ uint32(Size.X) * 4,
	                           /*SourceDepthPitch*/ uint32(Size.X) * uint32(Size.Y) * 4,
	                           SrcLocal);
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

		FVoxelGIVolumeSettings TestSettings;
		TestSettings.bEnabled = VoxelGIVolume::IsEnabled();
		TestSettings.DebugVis = VoxelGIVolume::GetDebugVis();
		TestSettings.OriginPoolUU = OriginPoolUU;
		TestSettings.FadeCentrePoolUU = FVector3f::ZeroVector;
		// No fade at all for the bring-up pattern: a checkerboard that dims
		// toward the edges is harder to read than one that does not, and the
		// point of this rung is addressing, not shading.
		TestSettings.FadeStartUU = 1.0e9f;
		TestSettings.FadeEndUU = 1.0e9f + 1.0f;

		ENQUEUE_RENDER_COMMAND(VoxelGIVolumeTest)(
			[TestSettings](FRHICommandListImmediate& RHICmdList)
		{
			GVoxelGIVolume.EnsureAllocated_RenderThread(RHICmdList);
			GVoxelGIVolume.FillCheckerboard_RenderThread(RHICmdList);
			GVoxelGIVolume.UpdateParameters_RenderThread(TestSettings);
		});
	}

	FAutoConsoleCommand GVoxelGIVolumeTestCmd(
		TEXT("voxel.GI.VolumeTest"),
		TEXT("Fill the GI volume with a per-brick checkerboard and bind it. Bring-up diagnostic: a crisp ")
		TEXT("3.2 m checker aligned to chunk boundaries proves the volume is created, bound, reachable from ")
		TEXT("the PIXEL shader, and correctly addressed. Needs voxel.GI.Volume 1."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&GIVolumeTestCommand));

	// THE SAME RUNG FOR VolumeLocal. Deliberately does NOT touch the origin or the
	// fade the way voxel.GI.VolumeTest does: by the time local lights are being
	// brought up the subsystem owns the origin and re-centres it per frame, and
	// stamping a bring-up origin over a live one would move the irradiance volumes
	// too. So this only fills and logs; the binding is whatever the subsystem last
	// published, which is the thing under test.
	void GIVolumeLocalTestCommand(const TArray<FString>& Args)
	{
		UE_LOG(LogTemp, Log,
		       TEXT("VoxelGI volume: LOCAL TEST requested (volume=%d localLights=%d localDim=%d debugVis=%d)"),
		       VoxelGIVolume::IsEnabled() ? 1 : 0, VoxelGIVolume::IsLocalEnabled() ? 1 : 0,
		       VoxelGIVolume::GetLocalDim(), VoxelGIVolume::GetDebugVis());

		// The cvar is read HERE, on the game thread the console command runs on, and
		// carried into the command. Reading it inside the render command instead would
		// return the render-thread shadow value, which the once-per-frame console sink
		// may not have updated yet -- so `voxel.GI.LocalLights 1` immediately followed
		// by `voxel.GI.VolumeLocalTest` would allocate nothing and print the
		// not-allocated warning, which reads as "the feature is broken".
		const bool bWantLocal = VoxelGIVolume::IsLocalEnabled();
		ENQUEUE_RENDER_COMMAND(VoxelGIVolumeLocalTest)(
			[bWantLocal](FRHICommandListImmediate& RHICmdList)
		{
			GVoxelGIVolume.EnsureAllocated_RenderThread(RHICmdList, bWantLocal);
			GVoxelGIVolume.FillLocalCheckerboard_RenderThread(RHICmdList);
		});
	}

	FAutoConsoleCommand GVoxelGIVolumeLocalTestCmd(
		TEXT("voxel.GI.VolumeLocalTest"),
		TEXT("Fill VolumeLocal's A channel with a per-brick checkerboard. The reachability rung for local ")
		TEXT("light injection: it is the only instrument that separates 'the bind/sample/display chain is ")
		TEXT("broken' from 'the splat never ran', and it catches both of the silent failures L1 can produce ")
		TEXT("-- a loose Texture3D declaration in the .ush (reads zeros forever) and a uniform member left ")
		TEXT("out of FVoxelGIVolumeSettings::operator== (latches off forever). Needs voxel.GI.Volume 1 and ")
		TEXT("voxel.GI.LocalLights 1. READ IT IN A CAVE, with voxel.GI.VolumeDebugVis 4 for the raw channel: ")
		TEXT("in open terrain Ambient is already 1 and max(Ambient, A) is a no-op by design. The NEXT splat ")
		TEXT("overwrites the pattern, so a torch placed afterwards is not fighting it."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&GIVolumeLocalTestCommand));
}

// --- P7 prerequisite: RDG registration, with a refusal ----------------------
//
// See the header for why this refuses rather than registering a sampled-only
// texture. The three conditions are checked in the order they can fail: the
// volumes may not be allocated yet (ordinary, on any frame before the first
// EnsureAllocated), and they may be allocated without a UAV (voxel.GI.VolumeUAV
// was 0 at startup, which is the default).
//
// LOGGED AT WARNING, ONCE PER PROCESS, AND NOT SILENT. A pass that skips itself
// because a prerequisite was off is exactly the shape that reads as "the
// feature ran and found nothing to do": this project has shipped several. The
// static flag keeps it off the per-frame log without letting it disappear.
bool FVoxelGIVolume::RegisterVolumesForCompute(FRDGBuilder& GraphBuilder,
                                               FRDGTextureRef& OutPos, FRDGTextureRef& OutNeg) const
{
	OutPos = nullptr;
	OutNeg = nullptr;

	if (!VolumePos.IsValid() || !VolumeNeg.IsValid())
	{
		return false;
	}
	if (!bAllocatedWithUAV)
	{
		static bool bLoggedNoUAV = false;
		if (!bLoggedNoUAV)
		{
			bLoggedNoUAV = true;
			UE_LOG(LogTemp, Warning,
			       TEXT("VoxelGI volume: RegisterVolumesForCompute REFUSED -- VolumePos/VolumeNeg were ")
			       TEXT("allocated WITHOUT ETextureCreateFlags::UAV, so no compute pass can write them. ")
			       TEXT("voxel.GI.VolumeUAV was 0 when the textures were created; it is STARTUP ONLY, so ")
			       TEXT("setting it now will not help this run. Restart with voxel.GI.VolumeUAV=1 on the ")
			       TEXT("COMMAND LINE. Refusing here rather than registering, because a non-UAV texture ")
			       TEXT("registers cleanly and then fails inside RDG at the caller's CreateUAV, which ")
			       TEXT("names the wrong cause."));
		}
		return false;
	}

	// .GetReference() rather than leaning on TRefCountPtr's implicit conversion,
	// so the argument type is the FRHITexture* the overload set expects and no
	// other overload can be selected by accident.
	OutPos = RegisterExternalTexture(GraphBuilder, VolumePos.GetReference(), TEXT("VoxelGI.IrradiancePos"));
	OutNeg = RegisterExternalTexture(GraphBuilder, VolumeNeg.GetReference(), TEXT("VoxelGI.IrradianceNeg"));
	return OutPos != nullptr && OutNeg != nullptr;
}
