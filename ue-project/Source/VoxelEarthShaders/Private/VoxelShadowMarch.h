// VoxelShadowMarch.h -- ray-marched SUN SHADOWS, fired from the depth buffer.
//
// S1 of docs/shadow-march-design-2026-08-20.md: measure the pass, touch
// nothing visible. One compute dispatch per view at
// PostRenderBasePassDeferred_RenderThread reconstructs a surface point from
// SceneDepth for every shaded pixel, offsets it one voxel along the GBufferA
// normal, and walks one ray toward the sun through the resident brick pyramid
// (level 0, the primary marcher's proven flat walk). The result is a
// screen-space visibility mask written to a TRANSIENT texture and a stats
// buffer read back for the census. Mode 2 (injection into the sun's screen
// shadow mask via a light function material) is S2 and is deliberately not
// here; asking for it logs once and runs as mode 1.
//
// WHY THIS HOOK. Scene depth and GBufferA are complete after the base pass
// (raster terrain via the prepass; marched terrain via the emit that runs
// earlier in this same hook), and RenderLights -- where S2's light function
// will consume the mask -- runs after it. The hook's signature hands us the
// scene-texture uniform buffer directly, which is the whole binding problem
// solved by the engine (the same argument VoxelMarchRenderer.h section 1 makes
// for the emit).
//
// WHY NOT INSIDE THE PRIMARY MARCH. Three reasons, recorded in the design doc:
// receivers (rays from the depth buffer shadow EVERY opaque pixel -- player
// proxy, agents, debris, thrown items -- not just terrain pixels), wave shape
// (a dependent secondary ray appended to the primary walk extends exactly the
// lanes that are already the tail), and territory (VoxelMarchRenderer.* is the
// primary-march workstream's). This pass has NO dependency on the primary
// marcher: with voxel.March 0 the depth buffer is raster terrain's and the
// shadow march works identically -- which is what makes S1 measurable against
// the shipping renderer.
//
// ORDERING AGAINST THE PRIMARY MARCHER'S EMIT: both extensions use this hook;
// extensions run sorted by GetPriority(), higher first (SceneViewExtension.h:
// 79,346). The marcher leaves the default 0; this one returns -10 to run after
// the emit, so marched terrain's depth is in SceneDepth before the shadow rays
// read it. In S1 legs (voxel.March 0) the ordering is moot.
//
// CROSS-THREAD SHAPE mirrors FVoxelMarchState: one state object shared between
// a game-thread owner (the subsystem below, which also feeds the sun
// direction) and the render-thread extension. Timing is an RQT_AbsoluteTime
// pair ring and stats are an FRHIGPUBufferReadback ring, both retired at
// PreRenderView BEFORE any gate -- the marcher's rule: an un-polled ring after
// the cvar drops to 0 reports "pending" forever, which reads exactly like "the
// pass never ran".

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "SceneViewExtension.h"
#include "Subsystems/WorldSubsystem.h"

#include "VoxelShadowMarch.generated.h"

class FRHIGPUBufferReadback;
class FTextureRenderTargetResource;
class UDirectionalLightComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UTextureRenderTarget2D;

// ---------------------------------------------------------------------------
// Cross-thread state
// ---------------------------------------------------------------------------
class FVoxelShadowMarchState
{
public:
	static constexpr int32 kNumTimingPairs = 4;
	static constexpr int32 kNumReadbacks = 4;
	// MIRRORS VoxelShadowMarch.usf's stat table (grep: VOXEL_SHADOW_STAT).
	static constexpr int32 kStatsWords = 64;

	mutable FCriticalSection Lock;

	// -- written by the game thread under Lock, read by the render thread --
	//
	// A DERIVED copy of the direction the sun light itself shades with, read
	// off the component once per game-thread tick ([[derived-not-verified-
	// detaches]] is the registered trap; the verify kernel's sun-nudge mutation
	// arm is what checks the pair against each other end to end). One frame of
	// latency against the light is accepted and recorded: the CSM path already
	// lags the sun by up to voxel.Sky.ShadowUpdateHz's whole period.
	FVector3f SunDirToSunWorld = FVector3f(0.0f, 0.0f, 1.0f);
	bool bSunValid = false;

	// -- the injection seam (S2), all under Lock -----------------------------
	//
	// The render thread WRITES DesiredMaskExtent (the scene buffer extent the
	// mask was built at this frame) and READS the published target; the game
	// thread owns the UTextureRenderTarget2D, sizes it to match, and publishes
	// its resource here. Publish/unpublish ordering on resize is load-bearing:
	// the subsystem NULLS the pointer BEFORE recreating the resource and only
	// republishes on a later tick, so the render thread can never dereference
	// a resource mid-recreation -- it declines instead, counted.
	FTextureRenderTargetResource* MaskTargetResource = nullptr; // game -> render
	FIntPoint MaskTargetExtent = FIntPoint::ZeroValue;          // game -> render
	FIntPoint DesiredMaskExtent = FIntPoint::ZeroValue;         // render -> game

	// -- render-thread-only ------------------------------------------------
	struct FTimingPair
	{
		FRenderQueryRHIRef Begin;
		FRenderQueryRHIRef End;
		bool bInFlight = false;
	};
	FTimingPair Timing[kNumTimingPairs];

	struct FReadbackSlot
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		bool bInFlight = false;
	};
	FReadbackSlot Readbacks[kNumReadbacks];

	// The inside-example dump (voxel.Shadow.MarchDiag): its own small ring, and
	// the last decoded payload held for the census printer. kDumpRecords * 8
	// words + 1 ticket word.
	static constexpr int32 kDumpRecords = 64;
	static constexpr int32 kDumpWords = 1 + kDumpRecords * 8;
	FReadbackSlot DumpReadbacks[2];
	TArray<uint32> LastDumpWords; // render-thread only; empty until first retire

	// The census window: sums of the per-frame stat buffers retired so far,
	// plus the timing observations. Reset after every periodic log line.
	uint64 WindowStats[kStatsWords] = {};
	uint32 WindowMaxSteps = 0;
	uint64 WindowFrames = 0; // readbacks RETIRED, not frames dispatched
	double WindowGpuMsSum = 0.0;
	float WindowGpuMsMax = 0.0f;
	uint32 WindowGpuMsCount = 0;
	float LastGpuMs = -1.0f;

	// Decline reasons, counted so "no rays marched" always names its cause --
	// zeros that look healthy are the failure mode the census exists to kill.
	uint64 DeclinedNoView = 0;
	uint64 DeclinedNoSun = 0;
	uint64 DeclinedNoPool = 0;
	uint64 DeclinedNoIndex = 0;
	uint64 DeclinedNoTextures = 0;
	uint64 MaskUnavailableForVerify = 0;

	// Injection census (render-thread-only, reset per window).
	uint64 InjectionCopies = 0;
	uint64 InjectionMutated = 0;
	uint64 DeclinedNoTarget = 0;
};

// ---------------------------------------------------------------------------
// The extension
// ---------------------------------------------------------------------------
class FVoxelShadowMarchExtension : public FWorldSceneViewExtension
{
public:
	FVoxelShadowMarchExtension(const FAutoRegister& AutoRegister, UWorld* InWorld,
	                           TSharedPtr<FVoxelShadowMarchState, ESPMode::ThreadSafe> InState);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	// After the primary marcher's emit (default priority 0) -- see the header
	// note on ordering.
	virtual int32 GetPriority() const override { return -10; }

	virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder,
	                                        FSceneView& InView) override;
	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder, FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

protected:
	virtual bool IsActiveThisFrame_Internal(
		const FSceneViewExtensionContext& Context) const override;

private:
	// The per-view stash, PreRenderView -> PostRenderBasePassDeferred. Aged by
	// frame number rather than unregistered, the marcher's reason verbatim: a
	// view culled between the hooks never says it went away.
	struct FViewStash
	{
		const FSceneView* ViewKey = nullptr;
		uint32 FrameNumber = 0;
		FIntRect ViewRect;
		FMatrix44f ViewToTranslatedWorld;
		FVector2f InvProjDiag = FVector2f::ZeroVector;
		float PixelConeSlope = 0.0f;
		FVector4f InvDeviceZToWorldZ = FVector4f::Zero();
		FVector ViewOriginUU = FVector::ZeroVector;
		FIntVector CameraVoxel = FIntVector::ZeroValue;
	};
	TArray<FViewStash, TInlineAllocator<4>> Views;

	TSharedPtr<FVoxelShadowMarchState, ESPMode::ThreadSafe> State;

	// Timing/readback retirement plus the periodic census line. Called at
	// PreRenderView, before any gate.
	void RetireGpuWork();
	void EmitCensusIfDue();
};

// ---------------------------------------------------------------------------
// The game-thread owner
// ---------------------------------------------------------------------------
//
// The marcher's extension is created by VoxelMarchPublishSource, called from
// the fluid publisher inside VoxelWorldSubsystem -- a file owned by another
// workstream. This subsystem exists so the shadow march needs no line there:
// it creates the extension at world begin-play and feeds the sun direction
// once per tick. It finds the sun the way the renderer would -- the
// directional light flagged as atmosphere sun light index 0 (which is what
// VoxelSkySubsystem::SpawnRig configures), falling back to the first
// directional light -- and logs the choice once. SEAM NOTE, routed to the sky
// workstream: if VoxelSkySubsystem ever respawns the sun actor mid-session,
// the cached weak pointer goes stale and this re-finds on the next tick; if it
// ever assigns a LIGHT FUNCTION to the sun, S2 needs to hear about it before
// it lands its own.
UCLASS()
class UVoxelShadowMarchSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;

private:
	// S2: keep the light-function wiring in step with the mode cvar. Wires the
	// sun's light function under mode 2 (material + MID + render target),
	// unwires on any other mode, and republishes the target after a resize.
	void UpdateInjection(int32 Mode);

	TSharedPtr<FVoxelShadowMarchState, ESPMode::ThreadSafe> State;
	TSharedPtr<FVoxelShadowMarchExtension, ESPMode::ThreadSafe> Extension;
	TWeakObjectPtr<UDirectionalLightComponent> SunComponent;
	bool bLoggedSunChoice = false;
	bool bLoggedNoSun = false;

	// S2 injection state. UPROPERTY so the GC cannot collect the target or the
	// MID out from under the sun's light function.
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> MaskTarget;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> LightFunctionParent;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> LightFunctionMID;
	bool bInjectionWired = false;
	bool bLoggedNoMaterial = false;
};
