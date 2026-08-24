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
//
// ---------------------------------------------------------------------------
// AND ONE LODGER: THE RAY-MARCH SPIKE (Phase 0 gate G1)
// ---------------------------------------------------------------------------
//
// docs/ray-marching-plan-2026-08-19.md §3's march spike lives inside this same
// extension, and the reason is that everything it needs is already here and
// nowhere else:
//
//   * the hook. PrePostProcessPass_RenderThread is the project's ONE sanctioned
//     post-opaque seam, for the reasons above -- including the RECORDED TRAP
//     that a standalone FRDGBuilder trips UE 5.8's breadcrumb sentinel assert
//     and kills the editor on the first Execute. A second extension would be a
//     second copy of that argument and a second chance to get it wrong.
//   * the volume. OccupancyKeepAlive already holds the 512^3 bit volume the
//     march walks; it is the same terrain the fluid collides against, built
//     from vxc::World including the edit overlay and never read back.
//   * the world gate and the scene textures, already resolved here.
//   * a working RQT_AbsoluteTime query ring with an in-flight pool, which has
//     already produced trusted numbers on this exact hook.
//
// It is a MEASUREMENT, not a feature: off unless voxel.Marcher.Spike is
// non-zero, and when it is off this file compiles to exactly what it was. It
// does not replace, disable or interact with the terrain raster path -- see
// VoxelMarchSpike.usf, which owns the whole argument for why an addition is
// the only composite that answers the gate's question.
//
// The spike DOES need voxel.Fluid.Enable 1, because that is what builds and
// fills the occupancy volume. Its own GPU time is bracketed separately
// (MarchGpuMs) so the solver's cost sitting in the same frame does not enter
// the number.

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIGPUReadback.h" // the ray census ring (voxel.Marcher.SpikeCount)
#include "SceneViewExtension.h"
#include "Templates/SharedPointer.h"
#include "VoxelFluidSim.h"  // FVoxelFluidSimTickArgs -- the sim-tick mailbox

class FVoxelFluidSimState;
class FVoxelFluidOccupancyVolume;

// THE MARCH SPIKE'S ARM, readable from any thread, straight off the cvars.
//
// WHY AN ACCESSOR AND NOT A STAT. The first leg printed `marchGpuMs=off` with
// voxel.Marcher.Spike 886 applied, because the budget was only ever published
// from inside the pass -- and the pass never ran (no occupancy volume, so the
// extension declined the frame). "Armed but could not run" landed in the "the
// operator asked for nothing" bucket, which is the exact confusion this field
// exists to prevent, and it was unfixable by publishing from anywhere on the
// render thread: every site there is downstream of the same gate that failed.
//
// So the ARM is asked of the cvars directly, by whoever is printing, and only
// the TIME comes back through FVoxelFluidRenderStats. A state that depends on
// a pass running can never again be used to describe whether it ran.
struct FVoxelMarchSpikeArm
{
	// 0 == voxel.Marcher.Spike is off. Otherwise the DDA step cap, clamped
	// exactly as the pass clamps it, so the printed arm is the arm that runs.
	int32 StepBudget = 0;
	bool bNoFetch = false;
	// voxel.Marcher.SpikeCount. Its own cvar, and its own RUN: the census
	// marches every ray a second time, so a frame that counts must never be
	// a frame whose marchGpuMs is quoted.
	bool bCount = false;

	// voxel.Marcher.SpikeSkip: 0 = the flat control, 1 = one mip level of
	// empty-space skipping, 2 = two. A SHADER PERMUTATION, not a uniform --
	// see the .usf for the measured reason (a runtime branch left the mip loads
	// in the no-fetch binary and silently re-based that arm).
	int32 SkipLevels = 0;
	// voxel.Marcher.SpikeSkipVerify: cross-check every skip ray against the
	// unit-tested flat walk, in the census pass only.
	bool bSkipVerify = false;
};
VOXELEARTHSHADERS_API FVoxelMarchSpikeArm VoxelMarchSpikeGetArm();

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


// The outcome codes, MIRRORED from VoxelMarchSpike.usf's
// VOXEL_MARCH_OUTCOME_* defines. Hand-maintained, exactly as this project
// mirrors worldgen.ush against amplifier.h -- and the failure mode of getting
// it wrong is not a crash but a census that attributes every miss to the hit
// column, so the histogram's row stride and its row MEANING are defined in one
// place on each side and nowhere else.
namespace VoxelMarchSpike
{
	inline constexpr int32 kOutcomeHit = 0;
	inline constexpr int32 kOutcomeMiss = 1;
	inline constexpr int32 kOutcomeExhausted = 2;
	inline constexpr int32 kOutcomeInside = 3;
	inline constexpr int32 kOutcomeNoBox = 4;
	inline constexpr int32 kOutcomeCount = 5;

	// One row above the outcomes, holding the skip-vs-flat verification
	// tallies. NOT an outcome and NOT part of the conservation sum -- see
	// VOXEL_MARCH_DIAG_ROW in the .usf.
	inline constexpr int32 kDiagRow = 5;
	inline constexpr int32 kHistRows = 6;
	inline constexpr int32 kDiagMismatchBin = 0;
	inline constexpr int32 kDiagComparedBin = 1;

	// The sample tail past the histogram rows -- mirror of
	// VOXEL_MARCH_DIAG_SAMPLE_* in the .usf.
	inline constexpr int32 kDiagSampleStride = 10;
	inline constexpr int32 kDiagSampleMax = 6;
	// Two header words ahead of the records: claimed slots, and rays excused
	// as a shared-face tie. The tie counter is here rather than in a diag
	// histogram bin because the budget-1 arm has only 2 bins.
	inline constexpr int32 kDiagSampleClaimed = 0;
	inline constexpr int32 kDiagSampleTies = 1;
	inline constexpr int32 kDiagSampleRefNoise = 2;
	inline constexpr int32 kDiagSampleRestart = 3;
	inline constexpr int32 kDiagSampleHeader = 4;
	inline constexpr int32 kDiagSampleWords =
		kDiagSampleHeader + kDiagSampleMax * kDiagSampleStride;
}

// THE RAY CENSUS (voxel.Marcher.SpikeCount 1), and what it is for.
//
// The first four arms gave four total-time points and a shape: linear in steps
// to 256, then the marginal cost collapsing ~4x. That is enough to see that
// almost every ray was still marching at 25.6 m and almost none survived past
// it -- but "3.18 ms to march 25.6 m" cannot be extrapolated to 4 km without
// knowing how many rays were alive at each depth and how far the average one
// actually got. Inferring a survival curve from four totals is precisely the
// derived-not-measured reasoning this project has been burned by, so the
// survival curve is measured instead.
//
// EXACT, NOT SAMPLED. The GPU builds a full histogram -- one bin per possible
// step count, per outcome -- and the CPU reads the whole thing back. Mean and
// p95 come off the distribution itself, so there is no bucketing error, no
// interpolation, and no accumulator that can overflow: a bin counts rays
// (at most ~1.4M at 1552x873) and never steps.
//
// Populated only while counting is on and a readback has landed; bValid is the
// ran-flag and is deliberately distinct from an all-zero census.
struct FVoxelMarchSpikeCensus
{
	bool bValid = false;
	// Monotonic, so a readback that lands out of order cannot overwrite a
	// newer census with an older one.
	uint64 Generation = 0;

	// The arm this census belongs to -- carried with the data, for the same
	// reason the timing carries it: a census joined to the wrong launch command
	// by hand is worse than no census.
	int32 StepBudget = 0;

	// Rays dispatched (the view rect's pixel count) and rays accounted for
	// (the sum of the five outcome counts). THESE MUST BE EQUAL. They are
	// printed together rather than checked silently, because a histogram that
	// is being written wrong still produces entirely plausible means.
	uint64 RaysDispatched = 0;
	uint64 RaysCounted = 0;

	// The five outcomes. Exhausted is NOT folded into missed: it is the
	// population a brick pyramid or a mip chain would rescue, and it is the
	// number the next traversal decision turns on.
	uint64 Hit = 0;
	uint64 Miss = 0;
	uint64 Exhausted = 0;
	uint64 Inside = 0;
	uint64 NoBox = 0;

	// Voxel advances per ray. Mean over ALL rays, then the tail -- the mean is
	// dominated by the early-terminating majority and on its own says almost
	// nothing about what the frame costs.
	float MeanSteps = 0.0f;
	float P95Steps = 0.0f;

	// And split by population, because sky rays and ground rays have completely
	// different cost profiles and every arm so far has been an average over
	// both of them.
	float MeanStepsHit = 0.0f;
	float MeanStepsMiss = 0.0f;
	float MeanStepsExhausted = 0.0f;

	// ---- the skip walk proving itself ---------------------------------
	// Rays where the skip walk and the unit-tested flat walk were comparable,
	// and rays where they DISAGREED. A non-zero mismatch invalidates the step
	// ratio the skip arms exist to produce; it rides the census so it cannot be
	// read without it. Zero compared means the verify arm did not run -- which
	// is not the same as agreement, and the log says so.
	uint64 SkipCompared = 0;
	uint64 SkipMismatch = 0;
	// Disagreements EXCUSED as a shared-face tie -- adjacent on one axis, both
	// solid, same point on the ray. Reported in its own column and never folded
	// into agreement: the class is understood, but a change in its rate is
	// still a signal. See THE TIE RULE in VoxelMarchSpike.usf.
	uint64 SkipTies = 0;
	// The REFERENCE's own instability, measured on the same rays in the same
	// frame: how often the unit-tested flat walk disagrees with a negligibly
	// perturbed copy of itself. mismatch is read against THIS, not against zero
	// -- see THE REFERENCE NOISE FLOOR in VoxelMarchSpike.usf.
	uint64 SkipRefNoise = 0;
	// THE DIRECT CONTROL, and the one the verdict uses: the same skip walk with
	// the mip forced solid -- identical restarts, provably zero skipping. Its
	// disagreements with flat are restart-induced by construction. It restarts
	// at every cell rather than only at occupied ones, so it is an UPPER bound:
	// mismatch above it is a hard result, mismatch below it is consistent with
	// noise but not proof of it.
	uint64 SkipRestartNoise = 0;
	int32 SkipLevels = 0;

	// A few of the disagreeing rays, so a non-zero mismatch is something to
	// debug rather than something to argue about. Pixel coordinates plus both
	// walks' answers -- see the sample tail's layout note in the .usf.
	struct FSample
	{
		uint32 PixelX = 0;
		uint32 PixelY = 0;
		bool bFlatFound = false;
		bool bSkipFound = false;
		FIntVector FlatVoxel = FIntVector::ZeroValue;
		FIntVector SkipVoxel = FIntVector::ZeroValue;
		uint32 SkipSteps = 0;
	};
	int32 SampleCount = 0;
	FSample Samples[VoxelMarchSpike::kDiagSampleMax];
};

// What the renderer reports back, 1 Hz-consumed by the subsystem's perf line.
// Same ran-flag discipline as FVoxelFluidCountsSnapshot: RenderGpuMs < 0
// means "no GPU timing has ever landed", FramesRendered == 0 means "the pass
// never ran" -- both deliberately distinct from a real 0.00.
struct FVoxelFluidRenderStats
{
	float RenderGpuMs = -1.0f;
	uint64 FramesRendered = 0;

	// --- the particle half of a toroidal recentre (contract item 8) ---------
	// Read against the subsystem's own recentre counter: a recentre with no
	// rebase is water teleported sideways relative to the terrain, so these are
	// meant to be read together and are printed together in the perf line.
	uint64 RebasePasses = 0;        // AddRebaseParticlesPass calls that ADDED a pass
	uint64 RebaseSlots = 0;         // particle slots those passes covered, summed
	// A delta was owed and particles existed, but no pass could be added (the
	// RHI refused it, or the buffers went away mid-flight). NOT the benign
	// "nothing has spawned yet" case, which is not counted at all: this one
	// means the water really is offset from the terrain by that delta.
	uint64 RebaseMissed = 0;
	// Sim ticks dropped because the origin moved AFTER the args were posted --
	// see PreRenderViewFamily_RenderThread. One frozen frame, deliberately,
	// instead of one frame of solving against terrain 6.4 m away.
	uint64 SimFramesStaleOrigin = 0;

	// --- the ray-march spike (Phase 0 gate G1) ------------------------------
	// THE NUMBER THE GATE EXISTS FOR: GPU milliseconds for the march pass
	// alone, from its own RQT_AbsoluteTime bracket, same shape and therefore
	// directly comparable to SimGpuMs and RenderGpuMs.
	//
	// The same ran-flag discipline as the two above, and it matters more here
	// than anywhere else in this struct: a spike that silently never ran must
	// never be able to print a small number. -1 means no GPU timing has ever
	// landed; MarchFrames == 0 means the pass was never added at all (no
	// occupancy volume, no scene textures, unsupported RHI); MarchStepBudget
	// == 0 means the cvar is off and nothing was asked for. All three are
	// distinct from a real 0.00, and the perf line prints them as words.
	float MarchGpuMs = -1.0f;
	uint64 MarchFrames = 0;
	// The arm this number belongs to, echoed back from what the RENDER THREAD
	// actually used -- not from what the cvar says on the game thread. Printed
	// on the same perf line as the time, because a measurement record that has
	// to be joined to a launch command by hand is a measurement record that
	// gets joined to the wrong one.
	int32 MarchStepBudget = 0;
	bool bMarchNoFetch = false;

	// The newest landed ray census. Independent of the timing above: the
	// census comes back through a buffer readback and the time through a
	// query, so one can be valid while the other is not, and each carries
	// its own ran-flag rather than borrowing the other's.
	FVoxelMarchSpikeCensus MarchCensus;
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

	// THE ORIGIN PendingSimArgs WAS BUILT AGAINST, and the reason it is here:
	// the volume's origin is game-thread state that a recentre moves MID-FRAME,
	// while the render thread is one frame behind consuming the PREVIOUS tick's
	// args. Without this stamp, the frame that straddles a recentre would solve
	// last tick's origin-relative boxes and un-rebased particles against a
	// volume bound at the NEW origin -- particles displaced up to a step
	// relative to the terrain, which the density constraint resolves by
	// ejecting them. Compared against the volume's live origin at consume time;
	// a mismatch drops the tick (counted, one frozen frame) and LEAVES the
	// rebase delta owed for the next one.
	FIntVector PendingSimArgsOriginVoxel = FIntVector::ZeroValue;

	// THE PARTICLE REBASE MAILBOX (contract item 8). Origin motion the particle
	// buffer still owes, in voxels, ACCUMULATED by the game thread
	// (TakePendingRebaseDeltaVoxels is itself take-and-clear, so a tick whose
	// args are never consumed must not lose its delta) and consumed exactly
	// once, with the args it belongs to, by PreRenderViewFamily_RenderThread.
	FIntVector PendingRebaseDeltaVoxels = FIntVector::ZeroValue;

	// -- written by the render thread under Lock --
	FVoxelFluidRenderStats Stats;

	// -- render-thread-only (no lock: only the two render-thread hooks touch
	//    these, and they run in order in the same frame) --
	struct FTimingPair
	{
		FRenderQueryRHIRef Begin;
		FRenderQueryRHIRef End;
		bool bInFlight = false;
	};
	FTimingPair TimingRing[kNumTimingPairs];

	// A SEPARATE ring for the march spike, not a shared one. The two passes
	// live in the same frame and the same hook, and a shared ring would give
	// whichever pass grabbed the free pair first -- so the reported number
	// would silently alternate between two different measurements. Four more
	// query pairs is the whole cost of that being impossible.
	FTimingPair MarchTimingRing[kNumTimingPairs];

	// ---- the ray census's readback ring (voxel.Marcher.SpikeCount) --------
	//
	// Same shape as the solver's FCountsReadback ring and for the same reason:
	// a GPU that is a frame or two behind must not stall the render thread, and
	// a slot that finds no free readback must be COUNTED rather than silently
	// dropping a frame's census. Three slots; the histogram is at most
	// 5 * (4096 + 1) uints = 80 KB, and typically 5 * 887 = 17 KB at the arms
	// the plan actually runs.
	static constexpr int32 kNumCensusReadbacks = 3;
	struct FCensusReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		// The bin count the histogram was BUILT with. Stored per slot because the
		// budget is a live cvar: a slot enqueued at budget 886 and read after a
		// flip to 64 would otherwise be decoded against the wrong stride and
		// produce a census that looks fine and describes nothing.
		int32 Bins = 0;
		int32 StepBudget = 0;
		int32 SkipLevels = 0;
		uint64 RaysDispatched = 0;
		uint64 Generation = 0;
		bool bInFlight = false;
	};
	FCensusReadback CensusRing[kNumCensusReadbacks];
	uint64 CensusGeneration = 0;
	// Frames that wanted a census and found no free slot. Non-zero means the
	// census is sampling rather than reporting every frame -- harmless for a
	// static pose, worth knowing before it is quoted as a per-frame figure.
	uint64 CensusSkips = 0;

	// THE ORIGIN THE PARTICLE BUFFER IS CURRENTLY EXPRESSED IN, world UU.
	// The splat needs the origin its positions actually use, which is NOT
	// necessarily Settings.FluidOriginWorld: the game thread publishes the new
	// origin the moment it recentres, but the buffer only moves when the rebase
	// pass runs. Written by PreRenderViewFamily_RenderThread on every sim-args
	// consume (which is exactly when the accompanying rebase lands), read by
	// PrePostProcessPass_RenderThread later in the same frame -- so a frame that
	// skipped the tick keeps drawing the water where the water still is.
	FVector ParticleOriginWorld = FVector::ZeroVector;
	bool bParticleOriginValid = false;

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

	// ANCHOR B OF THE RENDER-FRAME SPLIT, and no fluid work at all. See
	// VoxelRenderFrame.h. It is duplicated here and in the marcher for the same
	// reason the A anchor is: on a leg with voxel.March 0 the marcher's
	// extension declines every hook, and an anchor that lived in only one of
	// them would silently stop closing frames -- which reads exactly like the
	// instrument not being applied.
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder,
	                                               FSceneViewFamily& InViewFamily) override;
	//~ End ISceneViewExtension

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	// The Phase 0 G1 march spike. Called first thing in
	// PrePostProcessPass_RenderThread, ahead of and independent of the fluid
	// renderer's own gate: the spike needs the occupancy volume (which
	// voxel.Fluid.Enable builds) but NOT the particles, the settings, or
	// voxel.Fluid.Render. Adds nothing and touches nothing when
	// voxel.Marcher.Spike is 0.
	void AddMarchSpikePass(FRDGBuilder& GraphBuilder, const FSceneView& InView);

	TSharedPtr<FVoxelFluidRenderState, ESPMode::ThreadSafe> State;
};
