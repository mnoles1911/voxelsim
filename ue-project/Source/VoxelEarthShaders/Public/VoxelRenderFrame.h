// VoxelRenderFrame.h -- WHERE THE RENDER THREAD'S 18.60 ms GOES.
//
// ===========================================================================
// WHY THIS FILE EXISTS
// ===========================================================================
//
// VoxelFramePhase established, against the engine's own semantics, that this
// project is RENDERBOUND at every speed and in both motion states:
//
//     seg                    gameBusy  renderBusy  frame  floorFps  bound
//     M20 SETTLED-PARKED        1.69       9.23     9.32     108    RENDERBOUND
//     M20 SETTLED-MOVING       12.20      18.60    26.14      54    RENDERBOUND
//     M30 SETTLED-PARKED        1.87      10.12    10.25      99    RENDERBOUND
//     M30 SETTLED-MOVING       11.35      17.47    28.24      57    RENDERBOUND
//
// Parked, the render thread alone is a 108 fps ceiling with nothing streaming.
// Moving, it is a 54 fps floor -- deleting 100% of the remaining game-thread
// work cannot reach 100 fps. Goal 3 is a rendering problem.
//
// And renderBusy is ONE NUMBER. Nothing in this project has ever split it.
//
// ===========================================================================
// WHAT renderBusy ACTUALLY IS -- read from the engine, not assumed
// ===========================================================================
//
// SlateRHIRenderer.cpp:1356 (UE 5.8), at Present:
//
//     uint32 ThreadTime = EndTime - LastTimestamp;          // present-to-present
//     RenderThreadIdle  = FThreadIdleStats::Waits
//                       + GRenderThreadIdle[WaitingForGPUQuery]
//                       + GRenderThreadIdle[WaitingForGPUPresent];
//     GRenderThreadTime     = ThreadTime - RenderThreadIdle;
//     GRenderThreadWaitTime = RenderThreadIdle;
//
// So renderBusy is the render thread's CPU work, present-to-present, with all
// registered idle removed. IT IS NOT GPU TIME AND IT IS NOT A GPU WAIT. The
// marcher's existing timing brackets (VoxelMarchRenderer.cpp OpenBracket /
// CloseBracket) are RQT_AbsoluteTime GPU timestamps and measure a DIFFERENT
// AXIS ENTIRELY -- they can read near zero while this reads 18.60 and neither
// is wrong. Do not quote one against the other.
//
// ===========================================================================
// THE SPLIT, AND WHY THESE THREE BOUNDARIES
// ===========================================================================
//
// Read from the engine's call sites, not from the class diagram:
//
//   SceneRendering.cpp:4299     PreRenderViewFamily_RenderThread   <-- ANCHOR A
//   SceneRendering.cpp:4956     PostRenderViewFamily_RenderThread  <-- ANCHOR B
//   SceneRenderBuilder.cpp:916  GraphBuilder.Execute()
//   RenderGraphBuilder.cpp:2214 PostExecuteCallbacks fire          <-- ANCHOR E
//
// A and B bracket FSceneRenderer::Render, which in RDG is GRAPH CONSTRUCTION
// ONLY: visibility, GPU-scene update, mesh draw command setup, and every
// AddPass call in the frame. The pass LAMBDAS have not run yet. They run in
// Execute(), between B and E, and E fires after Execute has awaited its own
// parallel-translate tasks. So:
//
//     setupMs    A -> B   scene-renderer graph construction
//     executeMs  B -> E   RDG execute: recording RHI commands, plus the await
//                         on parallel translate tasks (that await is idle and
//                         is subtracted; see below)
//     tailMs     E -> A'  EVERYTHING ELSE THE RENDER THREAD DID. Named in full
//                         because a residual nobody names is a residual nobody
//                         believes: RDG resource flush and cleanup, Slate/UI
//                         draw, Present enqueue, EVERY OTHER RENDER COMMAND
//                         (brick-pool uploads, chunk-index uploads, proxy
//                         registration, readback polls), and the NEXT frame's
//                         scene update before the first extension hook.
//
// tailMs is not slop. It is the bucket most of this project's per-frame
// render-thread work is expected to land in, because none of that work is a
// scene-renderer pass. If it comes out large that is a FINDING, not a failure
// of the instrument, and it points outside this file's territory -- which is
// exactly what a residual is for.
//
// Inside setupMs, the extensions this workstream owns are timed by name:
//
//     mFam mView mBase mEmit   the marcher's four hooks
//     fluid                    the fluid render extension's hooks
//     shadow                   the shadow marcher's hooks (voxel.Shadow.March
//                              defaults 0, so this reads 0.00 on a stock leg
//                              and that is the CORRECT reading, not a dead one)
//     setupOtherMs             setupMs minus all of the above: the engine's own
//                              visibility / GPU scene / mesh draw command /
//                              shadow-depth / lighting / post setup, plus any
//                              extension this file does not instrument.
//
// ===========================================================================
// EVERY BUCKET IS BUSY, NOT WALL -- and that is what makes it reconcile
// ===========================================================================
//
// A wall-clock split does not add up to renderBusy; it adds up to the frame
// period, and the difference is idle. This project has already been burned by
// a share whose denominator was not the quantity it claimed to describe.
//
// So each anchor samples the SAME two counters the engine sums at Present:
//
//     UE::Stats::FThreadIdleStats::Get().Waits       (Core/Stats/ThreadIdleStats.h)
//     GRenderThreadIdle[WaitingForGPUQuery]          (RHI/RHICommandList.h)
//
// and every bucket subtracts its own share of them. GPUPresent idle accrues
// only at Present, which is inside tail, and is recovered from
// GRenderThreadWaitTime rather than sampled. The result is a split of BUSY
// time that can be compared to GRenderThreadTime directly.
//
// ===========================================================================
// THE RECONCILIATION DELTA IS PRINTED. A DRIFTING INSTRUMENT IS LOUD.
// ===========================================================================
//
//     reconDeltaMs = (setupBusy + executeBusy + tailBusy) - renderBusyMs
//
// printed in absolute ms AND as a share of renderBusyMs, both, because a
// percentage without its absolute is not a measurement and this file is not
// going to be the place that forgets it again.
//
// The delta is not decoration. These anchors bracket A->A' while the engine
// brackets Present->Present; those are the same period only if exactly one
// view family renders per frame and no frame is missed. When a scene capture
// adds a family, or a frame drops an extension hook, the delta grows and says
// so. THE BUCKETS MAY NOT BE QUOTED WHEN IT DOES.
//
//     |reconDeltaMs| > 15% of renderBusyMs   -> INSTRUMENT INVALID for that
//                                              segment. The line says
//                                              recon=INVALID in its own text.
//
// ===========================================================================
// REGISTERED DISPROOF -- WRITTEN BEFORE THE LEG, NOT AFTER
// ===========================================================================
//
// The question: renderBusy is 9.23 ms parked and 18.60 ms moving. Name the
// +9.4 ms that arrives BECAUSE THE CAMERA MOVES.
//
//   D0  THE INSTRUMENT IS VALID.
//       DISPROVED IF |reconDelta| > 15% of renderBusy in SETTLED-MOVING, or if
//       families/frame > 1.01, or if renderBusyMs reads 0.00.
//       If D0 fails, D1..D4 are unreadable and NOTHING below may be quoted.
//
//   D1  THE DELTA IS THE MARCHER'S OWN PASSES.
//       DISPROVED IF d(mFam+mView+mBase+mEmit) < 20% of d(renderBusy).
//       I EXPECT THIS TO BE DISPROVED and it is written down first. The
//       marcher's hooks add a fixed handful of passes whose count does not
//       depend on how many chunks are resident. If they measure ~0.2 ms in
//       both segments, then every "make the marcher cheaper" proposal is aimed
//       at a bucket that is not moving, and this instrument will have earned
//       its cost by killing those proposals rather than by finding a win.
//
//   D2  THE DELTA IS SCENE-RENDERER SETUP (setupOther).
//       DISPROVED IF d(setupOtherBusy) < 20% of d(renderBusy).
//
//   D3  THE DELTA IS RDG EXECUTE.
//       DISPROVED IF d(executeBusy) < 20% of d(renderBusy).
//
//   D4  THE DELTA IS OUTSIDE THE SCENE RENDERER ENTIRELY (tail).
//       CONFIRMED IF d(tailBusy) > 50% of d(renderBusy). Then the cost is
//       other render commands / Slate / present-adjacent work, this file says
//       so and NAMES NOTHING FURTHER from this leg, and the lane moves to
//       whoever owns those enqueues.
//
// D1..D4 CAN ALL BE DISPROVED AT ONCE. That is a legitimate outcome and it
// must be reported as "the render frame does not respond to motion in any
// bucket this instrument can see", not resolved by picking the largest.
//
// ===========================================================================
// THE MUTATION ARM -- because a check only ever observed passing is unproven
// ===========================================================================
//
// -VoxelRenderFrameMutate=N with -VoxelRenderFrameMutateMs=X (default 2.0).
// Six mutation arms exist in this codebase for exactly this purpose and none
// has ever been run, so this one states its expected RED reading up front:
//
//   1  BURN X ms of CPU inside the marcher's PreRenderBasePass hook.
//      EXPECT mBase +X, setupOther UNCHANGED, renderBusy +X, recon unchanged.
//      RED IF the extra time lands in setupOther instead of mBase -- the
//      sub-bucket scopes are not where they claim to be.
//
//   2  BURN X ms inside the post-execute callback.
//      EXPECT tail +X. RED IF it lands in execute -- anchor E is misplaced.
//
//   3  BLOCK (sleep) X ms inside PreRenderBasePass.
//      EXPECT mBase BUSY UNCHANGED, sveBlocked +X, renderWait +X, renderBusy
//      UNCHANGED. THIS IS THE ARM THAT PROVES THE IDLE CORRECTION. RED IF
//      mBase busy rises by X -- then every bucket in this file is wall time
//      wearing a busy label, and the reconciliation is a coincidence.
//
//   4  BURN X ms inside an RDG pass lambda (NeverCull, added at the marcher's
//      base-pass hook). EXPECT execute +X. RED IF it lands in setup -- then
//      RDG ran the lambda immediately rather than deferring it, and
//      setup/execute are not separable on this build.
//
// Arm 3 is the one to run first. It is the only one whose failure invalidates
// the whole file, and it is cheap.
//
// ===========================================================================
// FAILING READINGS, BOTH WAYS
// ===========================================================================
//
//   no "Voxel render frame" line at all, with -VoxelRenderFrame=1
//               -> the hooks were not applied, or the marcher extension never
//                  registered. NOTHING here ran. Check for "Voxel march view
//                  extension registered" in the same log.
//   frames=0 in a segment
//               -> THAT POPULATION IS EMPTY. A SETTLED-MOVING line with n=0
//                  means the leg never flew after settle, and the PARKED line
//                  may NEVER be quoted in its place. The owner's gate is the
//                  moving segment and a parked reading is not a pass.
//   recon=INVALID
//               -> see D0. The buckets do not describe this frame. Quoting one
//                  anyway is the same error as reading a percentage without
//                  its denominator.
//   families/frame > 1.01
//               -> more than one view family rendered per frame (scene
//                  capture, split screen). setupMs then swallows an entire
//                  intermediate Execute and the three-way split is NOT a
//                  partition. Reported as its own field so it cannot hide.
//   renderBusyMs=0.00
//               -> HARD ZERO. GRenderThreadTime is not populated in this
//                  configuration. It must NOT be read as "the render thread is
//                  idle"; it means the reconciliation target is dead and every
//                  share below it is divided by nothing.
//   dropped>0
//               -> frames where anchor A fired and anchor E never did. The
//                  sample is discarded rather than reconstructed. A large drop
//                  share means the graph is not executing on the path this
//                  file assumes.
//   camSpeedMS ~ 0 in the MOVING segment
//               -> the segmenter is reading a stationary camera while claiming
//                  motion. Invalid leg, not a fast one.
//   every bucket EQUAL between MOVING and PARKED
//               -> the render thread does not respond to motion anywhere this
//                  file can see. Say that. Do not pick the largest bucket.
//   LEVEL 2 ONLY, and the distinction is the whole point of the hit counters:
//   a group with h=0 and ms=0.000
//               -> A DEAD SCOPE, or a subsystem that did not run. It is NOT the
//                  same reading as h>0 with ms=0.000, which is a group that ran
//                  and cost nothing. Only the second may be reported as cheap.
//   chunkIndex h=0
//               -> EXPECTED on a stock leg and not a defect. Its only per-frame
//                  site is the GPU publish, and voxel.March.IndexGpuResident is
//                  off by default -- G10-M20's own log reads "publishes=0". The
//                  index's real per-frame cost is a game-thread
//                  QueueBufferUpload that this bucket cannot see, so a zero here
//                  is not evidence the chunk index is free.
//   poolComp h=0
//               -> EXPECTED under voxel.March 1. UVoxelGpuPoolComponent serves
//                  the QUAD renderer; marched terrain rides the brick pool. Same
//                  rule: not evidence it is free, evidence it is not in use.
//   tailOtherMs stays large
//               -> THE 29 INSTRUMENTED SITES DO NOT ACCOUNT FOR tail. The
//                  remainder is Slate, Present, RDG cleanup, or a render command
//                  nobody has instrumented. It is reported as unattributed and
//                  is NOT to be distributed into the groups that happen to be
//                  measured. The field is unclamped for exactly this reason.
//   l2OverheadMs a meaningful share of tailMs
//               -> the attribution arm is not a control for the D4 arm. That is
//                  why they are separate switch levels: run -VoxelRenderFrame=1
//                  for the D4 answer and =2 for the attribution, and never quote
//                  a tailMs from the =2 leg against a tailMs from the =1 leg
//                  without subtracting this.
//   shadow=0.00
//               -> CORRECT AND EXPECTED on a stock leg. voxel.Shadow.March
//                  defaults 0 and the extension declines IsActiveThisFrame, so
//                  not one hook is called. This is the file's own named DEAD
//                  READING and it is stated inline in the log text so nobody
//                  reports "shadows cost nothing" from it.
//
// AND THE WINDOW RULE, which has cost this project five confident wrong
// numbers: every line ends with win=%.2fs read from the SAME switch the rest
// of the harness uses (-VoxelPerfLogInterval), never a hardcoded 5000.0. Two
// hardcoded window divisors were found in two different files in one night;
// this file does not add a third. Read the segment lines, not the last line of
// the log -- the last window of a flight leg is the post-flight linger and it
// is PARKED.
#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;

namespace VoxelRenderFrame
{
// -VoxelRenderFrame=N, latched. 0 = OFF and off costs one predicted-not-taken
// branch on a latched int at each hook -- no timestamp, no counter, no
// allocation. An instrument armed on a hot path is a configuration change and
// this project has already paid 5% of a cold start for one; the cost here is
// O(1) per FRAME, not per chunk, and it is stated rather than assumed.
//
//   1  the render-frame split (this file's whole purpose)
//   2  1, PLUS TAIL ATTRIBUTION: the 29 ENQUEUE_RENDER_COMMAND sites that run
//      on the render thread outside the scene renderer, grouped by subsystem.
//
// SEPARABLE ON PURPOSE. The leg that answers D4 (is the parked->moving delta in
// tail at all?) and the leg that attributes tail are different legs. Level 1
// adds SIX scopes per frame; level 2 adds one per render command, and the
// render-command population is exactly what is under suspicion. If arming the
// attribution cost anything, folding it into the D4 answer would put the
// instrument inside its own measurement -- which is the -VoxelFineLockMeter
// mistake, and that one cost 5% of a cold start.
//
// WHAT LEVEL 2 COSTS, MEASURED RATHER THAN ASSERTED. The file calibrates its own
// scope at arm time (empty open/close pairs) and prints scopeCostNs, then prints
// l2Hits/frame and l2OverheadMs/frame = hits x scopeCostNs on every TAIL line.
// So the overhead is a printed number with its own absolute, not a claim -- and
// the TAIL line says so in its own text when it is a meaningful share of tailMs
// instead of leaving a reader to work it out.
VOXELEARTHSHADERS_API int32 Mode();

// Which named bucket a scope belongs to. Order is the print order.
enum class EBucket : uint8
{
	MarchFamily = 0,   // marcher Pre/PostRenderViewFamily_RenderThread
	MarchView,         // marcher PreRenderView_RenderThread
	MarchBase,         // marcher PreRenderBasePass_RenderThread
	MarchEmit,         // marcher PostRenderBasePassDeferred_RenderThread
	Fluid,             // fluid render extension hooks
	Shadow,            // shadow marcher hooks

	// ---- LEVEL 2 ONLY: the tail groups -------------------------------------
	//
	// These do NOT run inside the scene renderer. They are ENQUEUE_RENDER_COMMAND
	// bodies, which the render thread executes between scene renders -- i.e. in
	// the tail bucket -- and every one of them is driven by streaming. That is
	// the D4 mechanism, and these six buckets are what turn "tail is large" into
	// "tail is THIS".
	//
	// Grouped by owning file rather than per command, because the routing of a
	// fix is per file and because 29 separate buckets would be a log line nobody
	// reads. Per-command detail stays recoverable from the hit counters.
	TailGpuMeshJob,    // VoxelGpuMeshJobManager.cpp   7 sites
	TailBrickPool,     // VoxelBrickPool.cpp           4 sites
	TailChunkIndex,    // VoxelMarchChunkIndex.cpp     2 sites
	TailResidency,     // VoxelResidencyGpu.cpp        3 sites
	TailPoolComponent, // VoxelGpuPoolComponent.cpp    5 sites
	TailGIVolume,      // VoxelGI.cpp                  8 sites
	Num
};

// The first tail-group bucket, so the printer can tell the two halves apart
// without a second table that could drift out of step with the enum.
inline constexpr EBucket kFirstTailBucket = EBucket::TailGpuMeshJob;

// ANCHOR A. Called from the FIRST render-thread extension hook of the frame,
// by every extension this workstream owns. The first call of a given
// GFrameNumberRenderThread wins and closes the previous frame's sample; later
// calls in the same frame only count families. Safe to call from any of them
// in any order, which is the point -- extension iteration order is not ours to
// control and an anchor that depended on it would silently move.
VOXELEARTHSHADERS_API void Touch(FRDGBuilder& GraphBuilder);

// ANCHOR B. Called from PostRenderViewFamily_RenderThread. Last call of the
// frame wins.
VOXELEARTHSHADERS_API void NoteSetupEnd();

// Per-frame traffic, banked with the sample. A traffic counter before a timing
// one: if these do not move between parked and moving, no timing difference
// between them has a mechanism.
VOXELEARTHSHADERS_API void NoteView(const FVector& ViewOriginUU);
VOXELEARTHSHADERS_API void NoteMarchTiles(uint32 Tiles);

// RAII bucket scope. Samples cycles AND the two idle counters at both ends, so
// what it accumulates is BUSY time; the blocked remainder is accumulated
// separately into sveBlocked rather than being silently folded into busy.
struct VOXELEARTHSHADERS_API FScope
{
	// InMinMode gates the scope: 1 for the scene-renderer scopes, 2 for the tail
	// groups. A scope below the current mode costs one compare on a latched int
	// and never reads a clock.
	explicit FScope(EBucket InBucket, int32 InMinMode = 1);
	~FScope();

	FScope(const FScope&) = delete;
	FScope& operator=(const FScope&) = delete;

private:
	uint64 StartCycles = 0;
	uint32 StartIdle = 0;
	EBucket Bucket = EBucket::Num;
	bool bArmed = false;
};

// The mutation arms. See "THE MUTATION ARM" above. Called at the two sites the
// arms name; a no-op when -VoxelRenderFrameMutate is 0 or names another arm.
VOXELEARTHSHADERS_API void MutateHere(int32 ArmId);

// Which mutation arm is selected, so a call site can decide whether to build the
// arm's scaffolding at all.
//
// THIS EXISTS BECAUSE OF A RULE THIS PROJECT ALREADY PAID FOR. Arm 4 needs an
// RDG pass to burn inside, and adding that pass on every armed frame -- even
// with the arm off -- would make a measured build differ from an unmeasured one
// by a pass. `-VoxelFineLockMeter=1` cost 5% of a cold start for exactly that
// class of mistake, and "a leg carrying a meter is not a control for a leg that
// is not" is written down in the budget doc. So the pass is added ONLY when
// MutateArm() == 4, and the ordinary -VoxelRenderFrame=1 leg adds no pass at
// all.
VOXELEARTHSHADERS_API int32 MutateArm();
} // namespace VoxelRenderFrame

// The scope macro, so an armed build and an unarmed build differ by one branch
// rather than by a constructor call with an unused object.
#define VOXEL_RENDER_FRAME_SCOPE(BucketName) \
	VoxelRenderFrame::FScope PREPROCESSOR_JOIN(VoxelRenderFrameScope_, __LINE__)( \
		VoxelRenderFrame::EBucket::BucketName)

// THE TAIL-GROUP SCOPE. One line, first line of an ENQUEUE_RENDER_COMMAND
// lambda body, nothing else changing. Arms only at -VoxelRenderFrame=2.
//
// It is safe anywhere on the render thread: the scope banks into whichever of
// setup / execute / tail the frame is in WHEN IT CLOSES, so a command that ever
// ran inside the scene renderer would be booked to the right half rather than
// silently inflating tail. Nothing has to assume where these run.
#define VOXEL_RENDER_FRAME_SCOPE_TAIL(BucketName) \
	VoxelRenderFrame::FScope PREPROCESSOR_JOIN(VoxelRenderFrameTailScope_, __LINE__)( \
		VoxelRenderFrame::EBucket::BucketName, 2)
