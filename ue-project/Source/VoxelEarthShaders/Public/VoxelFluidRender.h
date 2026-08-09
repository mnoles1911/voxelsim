// VoxelFluidRender.h -- the screen-space fluid renderer for the PBF particles
// (docs/water-rearchitecture-plan-2026-08-09.md #6 fluid half / Phase 4;
// VoxelFluidRender.usf is the shader side and carries the pass-chain and
// material-ban documentation -- not restated here).
//
// THE COMPOSITING HOOK, AND WHY IT IS A SCENE VIEW EXTENSION. This project has
// never had a post-opaque render hook: every drawn thing so far is either a
// mesh pass primitive (the pooled quads, water pool, ribbons) or a CPU-fed
// resource (VoxelGIVolume). This file chooses the project's first one, and the
// choice is FSceneViewExtensionBase::PrePostProcessPass_RenderThread:
//
//   * It is the engine's sanctioned "add RDG passes against scene textures"
//     seam: it hands us the frame's FRDGBuilder plus FPostProcessingInputs,
//     whose SceneTextures uniform buffer carries SceneColorTexture (writable
//     RDG texture at this point) and SceneDepthTexture (opaque depth, exactly
//     the buffer the plan says to composite against). Engine plugins ship on
//     this exact hook (ColorCorrectRegions, CompositeCore -- verified in the
//     5.8 source tree), so it is a supported surface, not an accident.
//   * It runs AFTER all opaque + translucent scene rendering and BEFORE post
//     processing, so the fluid surface tone-maps/blooms with the scene like
//     the shipped water material does, and the opaque depth it reads is final.
//   * It works identically in -game and PIE; no editor-only machinery.
//   * The alternative -- a translucent material on a screen quad fed via a
//     material parameter collection -- cannot bind a StructuredBuffer of
//     particles at all (MPCs are scalars/vectors), would re-enter the very
//     material/sort-key machinery whose hazards the water material documents,
//     and would put the fluid BEFORE the translucency resolves it should not
//     participate in. It was considered and rejected on those grounds.
//
// FPostProcessingInputs lives in Runtime/Renderer/Internal/, which UBT exposes
// only to engine-scope modules, so the Inputs argument goes structurally
// UNUSED: both this header and the .cpp get by on the forward declaration
// SceneViewExtension.h provides, and the scene textures come from the public
// accessor UE::FXRenderingUtils::GetOrCreateSceneTextureUniformBuffer instead
// (FXRenderingUtils.h exists precisely for external-module render code; it
// returns the same uniform buffer Inputs carries at this point in the frame).
//
// ORDERING GUARANTEE (why the renderer sees this frame's particles): the
// subsystem enqueues the solver tick from the game thread inside its Tick,
// which runs before the frame's scene rendering is enqueued; render commands
// execute in order, so by the time PrePostProcessPass_RenderThread runs, the
// pooled particle buffer holds this frame's finalized positions. The renderer
// registers the SAME pooled buffer (FVoxelFluidSimState::Particles) into the
// scene's graph -- persistent pooled buffers crossing FRDGBuilder lifetimes is
// the state object's whole doctrine (VoxelFluidSim.h header comment).
//
// EVERYTHING HERE IS PRESENTATION. No readback, no authority, off by default
// (voxel.Fluid.Render 0); DrawDebugPoint remains the fallback view.

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "SceneViewExtension.h"
#include "Templates/SharedPointer.h"
#include "VoxelFluidSim.h"  // FVoxelFluidSimTickArgs -- the sim-tick mailbox

class FVoxelFluidSimState;
class FVoxelFluidOccupancyVolume;

// Game-thread-authored, render-thread-consumed settings. Marshalled by value
// under FVoxelFluidRenderState::Lock every subsystem tick; the render thread
// copies them out at pass-build time. No member is read without the lock.
struct FVoxelFluidRenderSettings
{
	// Master gate, mirrors voxel.Fluid.Render && voxel.Fluid.Enable && "the
	// sim has ever spawned". False = IsActiveThisFrame declines the frame and
	// zero GPU cost is paid.
	bool bEnabled = false;

	// The fluid origin in world UU (contract item 1: occupancy volume min
	// corner x 10). Double-precision here; the render thread folds it with the
	// view's PreViewTranslation into a camera-relative float3, which is where
	// the precision doctrine of the contract ("origin-relative to keep float
	// precision") meets the renderer.
	FVector FluidOriginWorld = FVector::ZeroVector;

	// Sprite radius (voxel.Fluid.Render.RadiusUU). 1.5x the 10 UU rest
	// spacing by default so neighbouring particles' impostors overlap into a
	// closed surface instead of a bag of marbles.
	float ParticleRadiusUU = 15.0f;

	// Bilateral blur radius in half-res pixels (voxel.Fluid.Render.SmoothRadiusPx).
	int32 SmoothRadiusPx = 6;

	// TOWARD the sun (VoxelEphemeris.h convention), world space, and the
	// day gate saturate(sunDir.z) -- both computed on the game thread from
	// UVoxelSkySubsystem so the renderer mirrors the water material's
	// constant-sky model with the same sun (VoxelFluidRender.usf, ban 2).
	FVector3f SunDirWorld = FVector3f(0.0f, 0.0f, 1.0f);
	float SunDayGate = 1.0f;
};

// What the renderer reports back, 1 Hz-consumed by the subsystem's perf line.
// Same ran-flag discipline as FVoxelFluidCountsSnapshot: RenderGpuMs < 0
// means "no GPU timing has ever landed", FramesRendered == 0 means "the pass
// never ran" -- both deliberately distinct from a real 0.00.
struct FVoxelFluidRenderStats
{
	float RenderGpuMs = -1.0f;
	uint64 FramesRendered = 0;
};

// Cross-thread state shared between the subsystem (game thread) and the view
// extension (render thread). Owned by TSharedPtr on both sides; contains no
// RHI resources except the timing queries, which die with the extension on
// the render thread (the registry's last frame snapshot keeps the extension
// alive through in-flight frames, the same lifetime argument the sim state
// documents for its render commands).
class VOXELEARTHSHADERS_API FVoxelFluidRenderState
{
public:
	// GPU timestamp pairs in flight, same shape as the sim's TimingRing.
	static constexpr int32 kNumTimingPairs = 4;

	mutable FCriticalSection Lock;

	// -- written by the game thread under Lock --
	FVoxelFluidRenderSettings Settings;
	// The solver state whose pooled particle buffer we splat. Refreshed every
	// tick so an Enable-cycle (which replaces the sim state object) re-links
	// the renderer to the new buffers automatically.
	TSharedPtr<FVoxelFluidSimState, ESPMode::ThreadSafe> SimState;

	// THE SIM-TICK MAILBOX. The subsystem posts at most one FVoxelFluidSimTickArgs
	// per game tick; PreRenderViewFamily_RenderThread consumes it exactly once
	// and adds the sim passes to the RENDERER'S graph. This replaced an
	// ENQUEUE_RENDER_COMMAND that built its own FRDGBuilder -- UE 5.8's RDG
	// breadcrumb sentinel assert killed the editor on the first standalone
	// Execute (RenderGraphBuilder.cpp:1772; see AddSimPasses' comment).
	// Consume-once also makes multi-view-family frames sim exactly once.
	TOptional<FVoxelFluidSimTickArgs> PendingSimArgs;
	// Lifetime anchor for PendingSimArgs->Occupancy (a raw pointer, same
	// pattern the old enqueue documented).
	TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe> OccupancyKeepAlive;

	// -- written by the render thread under Lock --
	FVoxelFluidRenderStats Stats;

	// -- render-thread-only (no lock: only PrePostProcessPass touches these) --
	struct FTimingPair
	{
		FRenderQueryRHIRef Begin;
		FRenderQueryRHIRef End;
		bool bInFlight = false;
	};
	FTimingPair TimingRing[kNumTimingPairs];

	FVoxelFluidRenderStats GetStats() const
	{
		FScopeLock Guard(&Lock);
		return Stats;
	}
};

// The extension itself. Created lazily by UVoxelFluidSubsystem via
// FSceneViewExtensions::NewExtension when voxel.Fluid.Render first goes to 1;
// FWorldSceneViewExtension scopes it to that subsystem's world, and
// IsActiveThisFrame_Internal declines every frame the settings say bEnabled
// is false, so an idle extension costs one lock per frame and nothing on the
// GPU.
class VOXELEARTHSHADERS_API FVoxelFluidRenderExtension : public FWorldSceneViewExtension
{
public:
	FVoxelFluidRenderExtension(const FAutoRegister& AutoRegister, UWorld* InWorld,
	                           TSharedPtr<FVoxelFluidRenderState, ESPMode::ThreadSafe> InState);

	//~ Begin ISceneViewExtension
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	// The sim rides the renderer's graph HERE, before scene rendering, so the
	// solver's writes are ordered before the splat pass reads them and before
	// anything else this frame samples the water.
	virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder,
	                                              FSceneViewFamily& InViewFamily) override;

	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView,
	                                             const FPostProcessingInputs& Inputs) override;
	//~ End ISceneViewExtension

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	TSharedPtr<FVoxelFluidRenderState, ESPMode::ThreadSafe> State;
};
