// VoxelRenderFrame.cpp -- the render-frame split. See VoxelRenderFrame.h for
// the whole argument: what renderBusy is, where the three anchors come from,
// the registered disproof, the mutation arms and the failing readings.
//
// This file contains no policy. It measures, it reconciles, and it prints the
// reconciliation delta so that a drifting instrument is loud rather than
// silent.

// ITS OWN HEADER FIRST. UnrealBuildTool enforces this and fails the build
// outright ("Expected VoxelRenderFrame.h to be first header included"); the
// <atomic> that used to sit above it made the header non-self-contained by
// accident, which is the whole thing the rule exists to catch. Surfaced when
// a new file in this module invalidated the makefile and forced a full IWYU
// re-validation -- it had been latent, not absent.
#include "VoxelRenderFrame.h"

#include <atomic>

#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RHICommandList.h"      // GRenderThreadIdle, ERenderThreadIdleTypes
#include "RenderGraphBuilder.h"
#include "RenderTimer.h"          // GRenderThreadTime / GRenderThreadWaitTime
#include "RenderingThread.h"      // GFrameNumberRenderThread, IsInRenderingThread
#include "Stats/ThreadIdleStats.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelRenderFrame, Log, All);

// Declared by VoxelMarchRenderer.cpp. The streaming subsystem already publishes
// its own convergence signal there every tick
// (VoxelWorldSubsystem.cpp:10403 -> VoxelMarchPublishStreamingState), so the
// settle boundary is READ from the system that owns it rather than re-derived
// here. A second opinion about the same word is how two instruments come to
// describe different worlds -- the same reason VoxelFramePhase borrowed the
// 100 UU/s moving threshold instead of inventing one.
VOXELEARTHSHADERS_API int32 VoxelMarchGetStreamConvergedFrames();
VOXELEARTHSHADERS_API bool VoxelMarchIsStreamConverged();

// Set once by NoteSettled(), read on the render thread. Atomic because the
// publisher is the game thread and the consumer is not.
static std::atomic<bool> GRenderFrameSettledLatch{false};

namespace VoxelRenderFrame
{
namespace
{
	// ---- the latched switches -------------------------------------------
	int32 LatchedMode()
	{
		static const int32 Latched = []
		{
			int32 Value = 0;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelRenderFrame="), Value);
			return FMath::Max(0, Value);
		}();
		return Latched;
	}

	int32 LatchedMutateArm()
	{
		static const int32 Latched = []
		{
			int32 Value = 0;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelRenderFrameMutate="), Value);
			return FMath::Max(0, Value);
		}();
		return Latched;
	}

	double LatchedMutateMs()
	{
		static const double Latched = []
		{
			float Value = 2.0f;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelRenderFrameMutateMs="), Value);
			return double(FMath::Max(0.0f, Value));
		}();
		return Latched;
	}

	// THE D0 GATE'S OWN MUTATION ARM. -VoxelRenderFrameFakeFamilies=N.
	//
	// A GATE THAT CANNOT FAIL IS NOT A GATE, and the families/frame gate has
	// now been on both sides of that: it read 2.00 on every leg on disk because
	// the count was "how many times Touch was called" rather than "how many
	// graphs were built", so D0 failed unconditionally and nothing below it was
	// ever quotable. Repairing it moves the risk to the opposite failure -- a
	// count that now reads 1.00 unconditionally would be equally useless and
	// would look healthy.
	//
	// So the repaired count has a runnable red arm. N>1 adds N-1 SYNTHETIC
	// families per frame and nothing else; the leg must then print
	// families/frame=N.00 and VERDICT=D0-FAILED. If it does not, the gate is
	// dead and the level-1 numbers under it may not be quoted. Default 0/1 =
	// no effect, and the ARMED line prints the value so a leg carrying the arm
	// can never be mistaken for a stock one.
	int32 LatchedFakeFamilies()
	{
		static const int32 Latched = []
		{
			int32 Value = 0;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelRenderFrameFakeFamilies="), Value);
			return FMath::Max(0, Value);
		}();
		return Latched;
	}

	// THE SAME SWITCH THE PRODUCER USES. Two hardcoded window divisors were
	// found in two different files in one night, both surviving the life of the
	// project, and every figure that divided by one of them was wrong by 2.5x.
	// This reads -VoxelPerfLogInterval, and the elapsed window is printed on
	// every line regardless, so a reader never has to trust this default.
	double LatchedWindowSeconds()
	{
		static const double Latched = []
		{
			float Value = 5.0f;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfLogInterval="), Value);
			return double(FMath::Clamp(Value, 0.5f, 120.0f));
		}();
		return Latched;
	}

	// THE MOVING THRESHOLD, IN UU/s. BORROWED, NOT INVENTED: 100.0 is the same
	// constant VoxelFramePhase segments on, which is itself the constant the
	// velocity-bias admission path tests against. Using a third number here
	// would let this file and the frame-phase file disagree about which frames
	// were moving while both looked correct.
	double LatchedMoveThresholdUU()
	{
		static const double Latched = []
		{
			float Value = 100.0f;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelFramePhaseMoveUU="), Value);
			return double(FMath::Max(0.0f, Value));
		}();
		return Latched;
	}

	// ---- cycle helpers ---------------------------------------------------
	FORCEINLINE double CyToMs(uint64 Cycles)
	{
		return double(Cycles) * FPlatformTime::GetSecondsPerCycle64() * 1000.0;
	}

	FORCEINLINE double Cy32ToMs(uint32 Cycles)
	{
		return double(Cycles) * FPlatformTime::GetSecondsPerCycle() * 1000.0;
	}

	// THE TWO IDLE COUNTERS THE ENGINE ITSELF SUMS AT PRESENT, read together so
	// a bucket subtracts exactly what SlateRHIRenderer will subtract later.
	// GPUPresent is deliberately NOT included: it accrues only inside Present,
	// which is in the tail bucket, and it is recovered from
	// GRenderThreadWaitTime rather than sampled -- sampling it here would
	// double-count it.
	FORCEINLINE uint32 SampleIdleCycles()
	{
		return UE::Stats::FThreadIdleStats::Get().Waits
		     + GRenderThreadIdle[ERenderThreadIdleTypes::WaitingForGPUQuery];
	}

	// ---- one accumulated population -------------------------------------
	//
	// EVERY FIELD IS A SUM OF PER-FRAME MILLISECONDS AND N IS THE FRAME COUNT,
	// so every printed figure is a per-frame mean with its own denominator.
	// Nothing here divides by a window length; the window length is printed
	// beside the line for the reader, not used inside it.
	struct FBucketSums
	{
		int64  N = 0;

		double SetupBusy = 0.0;
		double ExecuteBusy = 0.0;
		double TailBusy = 0.0;

		double Sub[int32(EBucket::Num)] = {};
		double SubHits[int32(EBucket::Num)] = {};
		// Busy banked by WHICH HALF OF THE FRAME the scope closed in, so each of
		// the three residuals subtracts exactly what was measured inside it and
		// nothing that was measured elsewhere. 0 = setup, 1 = execute, 2 = tail.
		double PhaseBusy[3] = {};
		double SveBlocked = 0.0;

		double PeriodWall = 0.0;
		double RenderBusy = 0.0;
		double RenderWait = 0.0;
		double RhiBusy = 0.0;

		double CamSpeedUU = 0.0;
		double Families = 0.0;
		// RAW Touch() CALLS. Families is the count of DISTINCT RDG graphs; this
		// is the count of anchor-A hook invocations that reached Touch. They are
		// NOT the same number and reading one as the other is the defect this
		// pair exists to make impossible to repeat: two extensions x one family
		// x (one family hook + one view hook) is four touches and ONE family.
		// Printed side by side so the ratio is visible rather than inferred.
		double Touches = 0.0;
		double Views = 0.0;
		double MarchTiles = 0.0;

		void Reset() { *this = FBucketSums(); }

		double M(double V) const { return N > 0 ? V / double(N) : 0.0; }

		double SubTotal() const
		{
			double S = 0.0;
			for (int32 i = 0; i < int32(EBucket::Num); ++i) { S += Sub[i]; }
			return S;
		}

		double TailGroupTotal() const
		{
			double S = 0.0;
			for (int32 i = int32(kFirstTailBucket); i < int32(EBucket::Num); ++i) { S += Sub[i]; }
			return S;
		}

		double TailGroupHits() const
		{
			double S = 0.0;
			for (int32 i = int32(kFirstTailBucket); i < int32(EBucket::Num); ++i) { S += SubHits[i]; }
			return S;
		}

		// ALL THREE RESIDUALS CAN COME OUT NEGATIVE AND NONE IS CLAMPED. A bucket
		// that cannot go negative is not a measurement: if the named scopes ever
		// exceed the span they sit inside, the split is wrong, and the only way a
		// reader finds that out is by seeing a minus sign.
		//
		// tailOther is the one the coordinator asked to stay explicit. If the 29
		// render-command sites do not account for tail, this number stays large
		// and SAYS SO, rather than the remainder being distributed into whatever
		// happens to be measured.
		double SetupOther()   const { return SetupBusy   - PhaseBusy[0]; }
		double ExecuteOther() const { return ExecuteBusy - PhaseBusy[1]; }
		double TailOther()    const { return TailBusy    - PhaseBusy[2]; }

		// THE RECONCILIATION. Positive means the buckets claim more busy time
		// than the engine says the thread had.
		double ReconDelta() const
		{
			return (SetupBusy + ExecuteBusy + TailBusy) - RenderBusy;
		}
	};

	// ---- process-wide state, render thread only --------------------------
	struct FState
	{
		// open frame
		bool   bFrameOpen = false;
		uint32 OpenFrameNumber = 0;
		uint64 CyA = 0;          // anchor A
		uint32 IdleA = 0;
		uint64 CyB = 0;          // anchor B (last family)
		uint32 IdleB = 0;
		bool   bHaveB = false;
		uint64 CyE = 0;          // anchor E (last post-execute)
		uint32 IdleE = 0;
		bool   bHaveE = false;
		int32  FamiliesThisFrame = 0;
		int32  TouchesThisFrame = 0;
		// THE GRAPH ANCHOR A IS CURRENTLY OPEN ON, and the whole of the
		// families/frame repair.
		//
		// Touch() is called from three hooks that this workstream owns
		// (VoxelMarchRenderer.cpp PreRenderViewFamily and PreRenderView,
		// VoxelFluidRender.cpp PreRenderViewFamily) and the second and third
		// exist on purpose -- they are idempotent safety anchors, because
		// extension iteration order is not ours to control and the marcher
		// extension declines every hook under voxel.March 0. Counting CALLS
		// therefore counted hooks, not families: every leg on disk read
		// families/frame=2.00 and failed D0 unconditionally.
		//
		// What actually breaks the three-way split is not a second hook, it is
		// a second GRAPH -- an intermediate GraphBuilder.Execute() inside the
		// A->B span, which is what an extra view family brings with it
		// (SceneRenderBuilder.cpp:873 constructs one FRDGBuilder per render node
		// and :916 executes it). So the count keys on the graph.
		//
		// Cleared by anchor E. That matters and is not decoration: the two
		// builders are stack objects in consecutive iterations of the same loop,
		// so the SECOND one is very likely to be handed the SAME ADDRESS as the
		// first. Pointer identity alone would therefore UNDERCOUNT -- the exact
		// direction that makes a gate unable to fail. E fires between them, so
		// the comparison is only ever made against a graph that is still live.
		const void* OpenGraph = nullptr;
		int32  ViewsThisFrame = 0;
		uint32 MarchTilesThisFrame = 0;
		double SubThisFrame[int32(EBucket::Num)] = {};
		double SubHitsThisFrame[int32(EBucket::Num)] = {};
		double PhaseBusyThisFrame[3] = {};
		double SveBlockedThisFrame = 0.0;
		FVector LastViewOrigin = FVector::ZeroVector;
		bool   bHaveLastViewOrigin = false;
		FVector ViewOriginThisFrame = FVector::ZeroVector;
		bool   bHaveViewOriginThisFrame = false;

		// counters
		int64 Dropped = 0;          // A fired, E never did
		int64 SkippedStartup = 0;   // engine globals describe history, not this frame
		int64 FramesSeen = 0;

		// settle latch. Once the streaming system has said converged, this run
		// is settled for good: during a flight the converged counter is reset to
		// zero every tick by the streaming that the flight causes, and reading
		// it live would classify the entire flight as UNSETTLED -- i.e. as cold
		// fill -- which is precisely the population confusion the segmentation
		// exists to prevent.
		bool bEverSettled = false;

		// populations
		FBucketSums MoveWindow, ParkWindow, FillWindow;
		FBucketSums MoveTotal, ParkTotal, FillTotal;

		// THE INSTRUMENT'S OWN COST, MEASURED AT ARM TIME rather than asserted.
		// Level 2 puts a scope on every render command, and the render-command
		// population is the thing under suspicion -- so its overhead is printed
		// with its own absolute beside the bucket it could contaminate.
		double ScopeCostNs = 0.0;

		double WindowStartSeconds = 0.0;
		double LastLogSeconds = 0.0;
		double LegStartSeconds = 0.0;
		bool   bStarted = false;
	};

	FState& Get()
	{
		static FState S;
		return S;
	}

	void BurnMs(double Ms)
	{
		// A burn the optimiser cannot delete: the accumulator escapes into a
		// volatile sink. A mutation arm that gets compiled away is a mutation
		// arm that proves nothing.
		static volatile double Sink = 0.0;
		const uint64 Start = FPlatformTime::Cycles64();
		double Acc = 0.0;
		int64 Spins = 0;
		while (CyToMs(FPlatformTime::Cycles64() - Start) < Ms)
		{
			Acc += double(++Spins) * 1.0000001;
		}
		Sink = Acc;
	}

	// THE INSTRUMENT'S OWN COST, MEASURED ONCE AT ARM TIME.
	//
	// Level 2 puts a scope on every render command, and the render-command
	// population is exactly what is under suspicion, so "the overhead is
	// negligible" is not something this file is entitled to assert. It times the
	// primitive operations an FScope open/close performs -- two Cycles64 reads,
	// two idle samples (a TLS lookup plus a global load each) -- and prints the
	// per-hit figure. The two accumulator adds are excluded and are the only
	// thing this understates; they are a pair of doubles.
	//
	// The loop uses the raw operations rather than real FScope objects on
	// purpose: constructing 4,096 real scopes would bank 4,096 hits into the
	// first frame's buckets, and an instrument whose calibration shows up in its
	// own first window is the failure mode this file exists to avoid.
	double CalibrateScopeNs()
	{
		constexpr int32 kIterations = 4096;
		static volatile uint64 CycleSink = 0;
		static volatile uint32 IdleSink = 0;
		const uint64 Start = FPlatformTime::Cycles64();
		for (int32 i = 0; i < kIterations; ++i)
		{
			const uint64 C0 = FPlatformTime::Cycles64();
			const uint32 I0 = SampleIdleCycles();
			const uint64 C1 = FPlatformTime::Cycles64();
			const uint32 I1 = SampleIdleCycles();
			CycleSink = C1 - C0;
			IdleSink = I1 - I0;
		}
		return (CyToMs(FPlatformTime::Cycles64() - Start) * 1.0e6) / double(kIterations);
	}

	// ANCHOR E, registered on ONE graph. Factored out of Touch() because a
	// second view family in the same frame builds a SECOND graph, and that
	// graph needs its own callback or CyE would still be the first graph's
	// Execute while B had already moved to the last family's -- an executeMs
	// that ended before the span it is supposed to close.
	//
	// LAST EXECUTE WINS, symmetrically with NoteSetupEnd()'s last-family-wins.
	// Registering on a graph also CLEARS OpenGraph when that graph finishes, so
	// the next Touch() is compared against nothing rather than against a dead
	// pointer that a later stack allocation may reuse.
	void RegisterAnchorE(FRDGBuilder& GraphBuilder)
	{
		GraphBuilder.AddPostExecuteCallback([]()
		{
			FState& St = Get();
			if (LatchedMutateArm() == 2)
			{
				BurnMs(LatchedMutateMs());
			}
			St.CyE = FPlatformTime::Cycles64();
			St.IdleE = SampleIdleCycles();
			St.bHaveE = true;
			// This graph is done. Anything that touches next is a new graph,
			// whatever address it happens to be given.
			St.OpenGraph = nullptr;
		});
	}

	// ---- HOW MANY CALL SITES EACH TAIL GROUP ACTUALLY HAS IN THIS BUILD ----
	//
	// h=0 IS TWO DIFFERENT READINGS AND THE LOG COULD NOT TELL THEM APART. The
	// header's own reading rules call h=0 "a DEAD SCOPE, or a subsystem that did
	// not run" -- and for the whole life of the instrument it was the first:
	// VOXEL_RENDER_FRAME_SCOPE_TAIL had ZERO call sites in the entire repository
	// (verified by unpiped count), so all six groups printed h=0 and the
	// DELTA-TAIL line printed GROUPS-DO-NOT-EXPLAIN-DTAIL for ANY input. That is
	// a confirmation that cannot come out the other way, which is not one.
	//
	// This table makes the distinction decidable at read time instead of leaving
	// it to a reader who would have to grep the build to find out. It is
	// hand-maintained, so it can drift -- and the drift is CHECKED rather than
	// trusted: a group with sites=0 that nevertheless records hits prints
	// TABLE-LIES, which is the direction that would otherwise make an UNWIRED
	// label hide real data.
	//
	// WHOEVER WIRES THE TWO LOCKED FILES MUST BUMP THIS. The counts below are
	// the identified sites, by ENQUEUE_RENDER_COMMAND name:
	//   TailGpuMeshJob  VoxelGpuMeshJobManager.cpp -- VoxelGpuQuadPayloadRelease,
	//                   VoxelGpuBrickStackRelease, VoxelGpuMeshReleaseReadbacks,
	//                   VoxelGpuMeshDispatchBatch, VoxelGpuMeshFetchQuads,
	//                   VoxelGpuMeshCompactQuads, VoxelGpuMeshPoll      (7)
	//   TailBrickPool   VoxelBrickPool.cpp -- VoxelGpuBrickPayloadRelease,
	//                   VoxelBrickPoolGpuFree, VoxelBrickPoolAllocWindow,
	//                   VoxelBrickPoolFlush                             (4)
	int32 TailSitesWired(EBucket B)
	{
		switch (B)
		{
		// WIRED 2026-08-25, after the two files came free. These are the two
		// groups most likely to carry the streaming tail, which is why the
		// instrument refuses to conclude anything while either reads 0 here.
		// Sites, by ENQUEUE_RENDER_COMMAND name (line numbers drift, names do not):
		//   TailGpuMeshJob: QuadPayloadRelease, BrickStackRelease,
		//     MeshReleaseReadbacks, MeshDispatchBatch, MeshFetchQuads,
		//     MeshCompactQuads, MeshPoll
		//   TailBrickPool: BrickPayloadRelease, BrickPoolGpuFree,
		//     BrickPoolAllocWindow, BrickPoolFlush
		// If a site is deleted or renamed without updating these counts, the log
		// prints tableCheck=TABLE-LIES rather than quietly under-reporting.
		case EBucket::TailGpuMeshJob:    return 7;
		case EBucket::TailBrickPool:     return 4;
		case EBucket::TailChunkIndex:    return 2;
		case EBucket::TailResidency:     return 3;
		case EBucket::TailPoolComponent: return 5;
		case EBucket::TailGIVolume:      return 8;
		default:                         return -1;
		}
	}

	int32 TailSitesWiredTotal()
	{
		int32 T = 0;
		for (int32 i = int32(kFirstTailBucket); i < int32(EBucket::Num); ++i)
		{
			T += FMath::Max(0, TailSitesWired(EBucket(i)));
		}
		return T;
	}

	int32 TailGroupsUnwired()
	{
		int32 U = 0;
		for (int32 i = int32(kFirstTailBucket); i < int32(EBucket::Num); ++i)
		{
			if (TailSitesWired(EBucket(i)) == 0) { ++U; }
		}
		return U;
	}

	// ---- printing --------------------------------------------------------
	void EmitSegment(const TCHAR* Name, const FBucketSums& B, double WindowSec)
	{
		if (B.N == 0)
		{
			UE_LOG(LogVoxelRenderFrame, Log,
			       TEXT("Voxel render frame seg=%s n=0 -- THIS POPULATION IS EMPTY. Nothing below ")
			       TEXT("describes it, no verdict may be drawn from it, and a non-empty segment on ")
			       TEXT("another line may NOT be quoted in its place. win=%.2fs"),
			       Name, WindowSec);
			return;
		}

		FState& S = Get();

		const double RenderBusy = B.M(B.RenderBusy);
		const double Setup   = B.M(B.SetupBusy);
		const double Execute = B.M(B.ExecuteBusy);
		const double Tail    = B.M(B.TailBusy);
		const double Other   = B.M(B.SetupOther());
		const double Recon   = B.M(B.ReconDelta());

		// EVERY SHARE BESIDE ITS OWN ABSOLUTE. waitShare=99.4% was true and
		// meaningless on this project because its denominator was 4.1 ms.
		auto Pct = [RenderBusy](double V) { return RenderBusy > 1e-9 ? 100.0 * V / RenderBusy : 0.0; };

		const bool bReconInvalid =
			(RenderBusy <= 1e-9) || (FMath::Abs(Recon) > 0.15 * RenderBusy);
		const double FamPerFrame = B.M(B.Families);
		const bool bMultiFamily = FamPerFrame > 1.01;

		UE_LOG(LogVoxelRenderFrame, Log,
		       TEXT("Voxel render frame seg=%s n=%lld renderBusyMs=%.2f periodMs=%.2f renderWaitMs=%.2f ")
		       TEXT("rhiMs=%.2f | setupMs=%.2f setupPct=%.0f executeMs=%.2f executePct=%.0f ")
		       TEXT("tailMs=%.2f tailPct=%.0f | recon=%s reconDeltaMs=%+.2f reconDeltaPct=%+.0f win=%.2fs"),
		       Name, (long long)B.N, RenderBusy, B.M(B.PeriodWall), B.M(B.RenderWait), B.M(B.RhiBusy),
		       Setup, Pct(Setup), Execute, Pct(Execute), Tail, Pct(Tail),
		       bReconInvalid ? TEXT("INVALID") : TEXT("ok"), Recon, Pct(Recon), WindowSec);

		// The setup half, named. setupOther is the engine's own scene-renderer
		// construction and is printed as a RESIDUAL of the named scopes, which
		// is why it is allowed to be negative.
		UE_LOG(LogVoxelRenderFrame, Log,
		       TEXT("Voxel render frame seg=%s SETUP setupMs=%.2f | mFamMs=%.3f mViewMs=%.3f ")
		       TEXT("mBaseMs=%.3f mEmitMs=%.3f fluidMs=%.3f shadowMs=%.3f marcherMs=%.3f ")
		       TEXT("setupOtherMs=%.2f setupOtherPct=%.0f sveBlockedMs=%.3f -- shadowMs=0.000 is the ")
		       TEXT("EXPECTED reading (voxel.Shadow.March defaults 0, the extension declines every ")
		       TEXT("frame and no hook is called); it is NOT evidence that shadows are free. ")
		       TEXT("setupOther is a RESIDUAL and may print negative -- if it does, the named scopes ")
		       TEXT("exceed the A->B span and the split is wrong. win=%.2fs"),
		       Name, Setup,
		       B.M(B.Sub[int32(EBucket::MarchFamily)]), B.M(B.Sub[int32(EBucket::MarchView)]),
		       B.M(B.Sub[int32(EBucket::MarchBase)]),   B.M(B.Sub[int32(EBucket::MarchEmit)]),
		       B.M(B.Sub[int32(EBucket::Fluid)]),       B.M(B.Sub[int32(EBucket::Shadow)]),
		       B.M(B.Sub[int32(EBucket::MarchFamily)] + B.Sub[int32(EBucket::MarchView)]
		           + B.Sub[int32(EBucket::MarchBase)] + B.Sub[int32(EBucket::MarchEmit)]),
		       Other, Pct(Other), B.M(B.SveBlocked), WindowSec);

		// ---- THE TAIL ATTRIBUTION, level 2 only -------------------------
		//
		// tailMs is where the D4 prior says the parked->moving delta lives: 29
		// ENQUEUE_RENDER_COMMAND sites that the render thread executes BETWEEN
		// scene renders, every one of them driven by streaming. These six groups
		// are what turn "tail is large" into "tail is THIS".
		if (LatchedMode() >= 2)
		{
			const double TailOther = B.M(B.TailOther());
			const double GroupTotal = B.M(B.TailGroupTotal());
			const double Hits = B.M(B.TailGroupHits());
			const double OverheadMs = Hits * S.ScopeCostNs / 1.0e6;
			auto TailPct = [Tail](double V) { return Tail > 1e-9 ? 100.0 * V / Tail : 0.0; };

			UE_LOG(LogVoxelRenderFrame, Log,
			       TEXT("Voxel render frame seg=%s TAIL tailMs=%.2f | meshJobMs=%.3f(h=%.1f) ")
			       TEXT("brickPoolMs=%.3f(h=%.1f) chunkIndexMs=%.3f(h=%.1f) residencyMs=%.3f(h=%.1f) ")
			       TEXT("poolCompMs=%.3f(h=%.1f) giVolMs=%.3f(h=%.1f) | groupTotalMs=%.2f (%.0f%% of tail) ")
			       TEXT("tailOtherMs=%.2f (%.0f%% of tail) execOtherMs=%.2f | l2Hits/frame=%.1f ")
			       TEXT("scopeCostNs=%.1f l2OverheadMs=%.3f (%.1f%% of tail) win=%.2fs"),
			       Name, Tail,
			       B.M(B.Sub[int32(EBucket::TailGpuMeshJob)]),    B.M(B.SubHits[int32(EBucket::TailGpuMeshJob)]),
			       B.M(B.Sub[int32(EBucket::TailBrickPool)]),     B.M(B.SubHits[int32(EBucket::TailBrickPool)]),
			       B.M(B.Sub[int32(EBucket::TailChunkIndex)]),    B.M(B.SubHits[int32(EBucket::TailChunkIndex)]),
			       B.M(B.Sub[int32(EBucket::TailResidency)]),     B.M(B.SubHits[int32(EBucket::TailResidency)]),
			       B.M(B.Sub[int32(EBucket::TailPoolComponent)]), B.M(B.SubHits[int32(EBucket::TailPoolComponent)]),
			       B.M(B.Sub[int32(EBucket::TailGIVolume)]),      B.M(B.SubHits[int32(EBucket::TailGIVolume)]),
			       GroupTotal, TailPct(GroupTotal),
			       TailOther, TailPct(TailOther), B.M(B.ExecuteOther()),
			       Hits, S.ScopeCostNs, OverheadMs, TailPct(OverheadMs), WindowSec);

			// FAILING READINGS FOR THIS LINE, BOTH WAYS, PER GROUP -- stated in
			// the line's own text because that is the cheapest defence this
			// project has found, and because two of these six groups are
			// EXPECTED to read zero on a stock leg for reasons that have nothing
			// to do with them being cheap.
			UE_LOG(LogVoxelRenderFrame, Log,
			       TEXT("Voxel render frame seg=%s TAIL-READING -- h=0 with ms=0.000 is a DEAD SCOPE ")
			       TEXT("or a subsystem that did not run, and is NOT the same reading as h>0 with ")
			       TEXT("ms=0.000, which is a group that RAN AND COST NOTHING. Only the second may be ")
			       TEXT("reported as cheap. TWO GROUPS ARE EXPECTED TO READ h=0 HERE and neither is a ")
			       TEXT("defect: chunkIndex, because its only per-frame site is the GPU publish and ")
			       TEXT("voxel.March.IndexGpuResident is off by default (the leg's own line reads ")
			       TEXT("'publishes=0'), so the index's real cost is a game-thread QueueBufferUpload ")
			       TEXT("this bucket cannot see; and poolComp, because it serves the QUAD renderer and ")
			       TEXT("under voxel.March 1 terrain rides the brick pool instead. A zero from either ")
			       TEXT("is NOT evidence that the subsystem is free. tailOtherMs is UNCLAMPED and is ")
			       TEXT("the honest answer to 'do the 29 sites account for tail': if it stays large, ")
			       TEXT("they do not, and the remainder is Slate, Present, RDG cleanup or a render ")
			       TEXT("command nobody has instrumented -- it is NOT to be distributed into the ")
			       TEXT("groups that happen to be measured. win=%.2fs"),
			       Name, WindowSec);

			// WHICH h=0 IS WHICH. The reading rule above says h=0 is EITHER a
			// dead scope OR a subsystem that did not run; this line says which,
			// per group, from the build rather than from a guess -- and checks
			// its own table against the hits so an UNWIRED label cannot hide
			// real data.
			FString Wiring;
			bool bTableLies = false;
			static const TCHAR* const GroupNames[] = {
				TEXT("meshJob"), TEXT("brickPool"), TEXT("chunkIndex"),
				TEXT("residency"), TEXT("poolComp"), TEXT("giVol") };
			for (int32 i = int32(kFirstTailBucket); i < int32(EBucket::Num); ++i)
			{
				const int32 Sites = TailSitesWired(EBucket(i));
				const double GroupHits = B.SubHits[i];
				if (Sites == 0 && GroupHits > 0.0) { bTableLies = true; }
				Wiring += FString::Printf(
					TEXT("%s=%d%s(hits=%.0f/frames=%lld) "),
					GroupNames[i - int32(kFirstTailBucket)], Sites,
					Sites == 0 ? TEXT("-UNWIRED-h0-IS-A-DEAD-SCOPE")
					           : TEXT("-wired-h0-MEANS-IT-DID-NOT-RUN"),
					GroupHits, (long long)B.N);
			}

			UE_LOG(LogVoxelRenderFrame, Log,
			       TEXT("Voxel render frame seg=%s TAIL-WIRING sitesWired=%d groupsUnwired=%d ")
			       TEXT("| %s| tableCheck=%s -- an UNWIRED group's h=0 says NOTHING about that ")
			       TEXT("subsystem's cost; it says this build contains no scope for it. Both ")
			       TEXT("unwired groups (meshJob, brickPool) live in files that were locked when ")
			       TEXT("this instrument was repaired, and they are the two most likely to carry ")
			       TEXT("the streaming tail -- so while groupsUnwired>0 the DELTA-TAIL line's ")
			       TEXT("negative verdict is NOT DECIDABLE and may not be quoted as 'streaming ")
			       TEXT("render commands do not explain dTail'. tableCheck=TABLE-LIES means a ")
			       TEXT("group marked UNWIRED recorded hits: the table above is stale, trust the ")
			       TEXT("hits and fix the table. win=%.2fs"),
			       Name, TailSitesWiredTotal(), TailGroupsUnwired(), *Wiring,
			       bTableLies ? TEXT("TABLE-LIES") : TEXT("ok"), WindowSec);
		}

		// TRAFFIC BEFORE TIMING. If these do not move between parked and moving
		// then no timing difference between the two has a mechanism, and any
		// bucket delta is asking to be explained by something this file cannot
		// see.
		UE_LOG(LogVoxelRenderFrame, Log,
		       TEXT("Voxel render frame seg=%s TRAFFIC families/frame=%.2f%s (graphs=%.0f / frames=%lld) ")
		       TEXT("touches/frame=%.2f (touches=%.0f / frames=%lld) views/frame=%.2f (views=%.0f / frames=%lld) ")
		       TEXT("fakeFamilyArm=%d marchTiles/frame=%.0f camSpeedMS=%.1f framesTotal=%lld dropped=%lld ")
		       TEXT("skippedStartup=%lld settleLatched=%s -- EVERY RATE ON THIS LINE CARRIES ITS OWN ")
		       TEXT("NUMERATOR AND DENOMINATOR, because two counters were misread in one night for ")
		       TEXT("lack of one. families/frame COUNTS DISTINCT RDG GRAPHS, NOT Touch() CALLS: three ")
		       TEXT("hooks take the anchor (marcher family, marcher view, fluid family) and a healthy ")
		       TEXT("one-family frame therefore reads touches/frame 1.00-3.00 with families/frame 1.00. ")
		       TEXT("families/frame above 1.01 means a second graph EXECUTED inside the A->B span, ")
		       TEXT("setupMs swallowed that Execute and the three-way split is NOT a partition; ")
		       TEXT("touches/frame above families/frame is NORMAL and is not that. If families/frame ")
		       TEXT("reads 1.00 on every leg ever run, the gate is unproven -- run one leg with ")
		       TEXT("-VoxelRenderFrameFakeFamilies=2 and confirm it reads 2.00 and D0 FAILS. ")
		       TEXT("camSpeedMS near zero on a MOVING line means the segmenter is wrong and the leg ")
		       TEXT("is invalid, not fast. win=%.2fs"),
		       Name, FamPerFrame, bMultiFamily ? TEXT(" MULTI-FAMILY-SPLIT-NOT-A-PARTITION") : TEXT(""),
		       B.Families, (long long)B.N,
		       B.M(B.Touches), B.Touches, (long long)B.N,
		       B.M(B.Views), B.Views, (long long)B.N,
		       LatchedFakeFamilies(),
		       B.M(B.MarchTiles), B.M(B.CamSpeedUU) / 100.0,
		       (long long)S.FramesSeen, (long long)S.Dropped, (long long)S.SkippedStartup,
		       S.bEverSettled ? TEXT("yes") : TEXT("NO"),
		       WindowSec);
	}

	// ONE DELTA, ONE VERDICT. The same arithmetic and the same thresholds as
	// the registered disproof in the header, evaluated by the instrument rather
	// than by a reader afterwards -- so the verdict cannot be renegotiated once
	// the numbers are visible.
	void EmitDelta(const TCHAR* Tag, const FBucketSums& Hi, const FBucketSums& Lo, double WindowSec)
	{
		if (Hi.N == 0 || Lo.N == 0)
		{
			UE_LOG(LogVoxelRenderFrame, Log,
			       TEXT("Voxel render frame DELTA tag=%s VERDICT=NOT-COMPUTED nMoving=%lld nParked=%lld ")
			       TEXT("-- a window without BOTH populations says nothing about the comparison. ")
			       TEXT("win=%.2fs"),
			       Tag, (long long)Hi.N, (long long)Lo.N, WindowSec);
			return;
		}

		const double DRender  = Hi.M(Hi.RenderBusy)  - Lo.M(Lo.RenderBusy);
		const double DSetup   = Hi.M(Hi.SetupBusy)   - Lo.M(Lo.SetupBusy);
		const double DExec    = Hi.M(Hi.ExecuteBusy) - Lo.M(Lo.ExecuteBusy);
		const double DTail    = Hi.M(Hi.TailBusy)    - Lo.M(Lo.TailBusy);
		const double DOther   = Hi.M(Hi.SetupOther())- Lo.M(Lo.SetupOther());
		const double DMarcher =
			(Hi.M(Hi.Sub[int32(EBucket::MarchFamily)] + Hi.Sub[int32(EBucket::MarchView)]
			     + Hi.Sub[int32(EBucket::MarchBase)]  + Hi.Sub[int32(EBucket::MarchEmit)]))
		  - (Lo.M(Lo.Sub[int32(EBucket::MarchFamily)] + Lo.Sub[int32(EBucket::MarchView)]
			     + Lo.Sub[int32(EBucket::MarchBase)]  + Lo.Sub[int32(EBucket::MarchEmit)]));

		auto Share = [DRender](double V) { return FMath::Abs(DRender) > 1e-9 ? 100.0 * V / DRender : 0.0; };

		// D0 first: if the instrument is invalid the rest is unreadable.
		const double HiRecon = Hi.M(Hi.ReconDelta());
		const double HiBusy  = Hi.M(Hi.RenderBusy);
		const double HiFam = Hi.M(Hi.Families);
		const bool bD0Busy  = HiBusy > 1e-9;
		const bool bD0Recon = bD0Busy && FMath::Abs(HiRecon) <= 0.15 * HiBusy;
		const bool bD0Fam   = HiFam <= 1.01;
		const bool bD0 = bD0Busy && bD0Recon && bD0Fam;
		// WHICH of the three D0 clauses failed, named in the line. D0 read
		// FAILED on every leg on disk and the line did not say which clause did
		// it, so the whole breakdown was unquotable without anyone being able to
		// see that the cause was a hook counter rather than the split.
		const TCHAR* D0Why =
			  !bD0Busy  ? TEXT("renderBusy=0-GRenderThreadTime-not-populated")
			: !bD0Fam   ? TEXT("families/frame>1.01-a-second-graph-executed-inside-A..B")
			: !bD0Recon ? TEXT("recon-delta-over-15pct")
			:             TEXT("-");

		const bool bD1 = FMath::Abs(DMarcher) >= 0.20 * FMath::Abs(DRender);
		const bool bD2 = FMath::Abs(DOther)   >= 0.20 * FMath::Abs(DRender);
		const bool bD3 = FMath::Abs(DExec)    >= 0.20 * FMath::Abs(DRender);
		const bool bD4 = DTail                >  0.50 * DRender;

		const TCHAR* Verdict =
			!bD0 ? TEXT("D0-FAILED-INSTRUMENT-INVALID-NOTHING-BELOW-MAY-BE-QUOTED")
			: bD4 ? TEXT("D4-CONFIRMED-OUTSIDE-THE-SCENE-RENDERER")
			: (bD1 && !bD2 && !bD3) ? TEXT("D1-THE-MARCHERS-OWN-PASSES")
			: (bD2 && !bD1 && !bD3) ? TEXT("D2-SCENE-RENDERER-SETUP")
			: (bD3 && !bD1 && !bD2) ? TEXT("D3-RDG-EXECUTE")
			: (!bD1 && !bD2 && !bD3 && !bD4)
				? TEXT("ALL-DISPROVED-NO-BUCKET-RESPONDS-TO-MOTION")
				: TEXT("SPLIT-ACROSS-BUCKETS-NAME-ALL-OF-THEM-NOT-THE-LARGEST");

		UE_LOG(LogVoxelRenderFrame, Log,
		       TEXT("Voxel render frame DELTA tag=%s VERDICT=%s nMoving=%lld nParked=%lld ")
		       TEXT("dRenderBusyMs=%+.2f | dSetupMs=%+.2f (%.0f%%) dExecuteMs=%+.2f (%.0f%%) ")
		       TEXT("dTailMs=%+.2f (%.0f%%) | dMarcherMs=%+.3f (%.0f%%) dSetupOtherMs=%+.2f (%.0f%%) ")
		       TEXT("| D0=%s(%s famPerFrame=%.2f over n=%lld) D1=%s D2=%s D3=%s D4=%s -- thresholds are the ones registered in ")
		       TEXT("VoxelRenderFrame.h BEFORE this leg ran; D1 was expected to be DISPROVED. ")
		       TEXT("All four disproved at once is a legitimate result and must be reported as ")
		       TEXT("such, not resolved by picking the largest bucket. win=%.2fs"),
		       Tag, Verdict, (long long)Hi.N, (long long)Lo.N, DRender,
		       DSetup, Share(DSetup), DExec, Share(DExec), DTail, Share(DTail),
		       DMarcher, Share(DMarcher), DOther, Share(DOther),
		       bD0 ? TEXT("ok") : TEXT("FAILED"), D0Why, HiFam, (long long)Hi.N,
		       bD1 ? TEXT("held") : TEXT("disproved"),
		       bD2 ? TEXT("held") : TEXT("disproved"),
		       bD3 ? TEXT("held") : TEXT("disproved"),
		       bD4 ? TEXT("CONFIRMED") : TEXT("not-confirmed"),
		       WindowSec);

		// ---- D4's ATTRIBUTION, level 2 only ------------------------------
		//
		// D4 says the delta is outside the scene renderer. This line says WHICH
		// SUBSYSTEM, and it is the number that decides whether the render-thread
		// ceiling and the chunks/s ceiling are the same problem: every one of
		// these six groups is streaming work paid on the render thread.
		//
		// dTailOther is printed beside them and is NOT clamped. If it carries
		// most of dTail then the 29 instrumented sites are NOT the mechanism,
		// and that must be reported as such rather than by naming the largest of
		// six small groups.
		if (LatchedMode() >= 2)
		{
			const double DGroup = Hi.M(Hi.TailGroupTotal()) - Lo.M(Lo.TailGroupTotal());
			const double DOtherTail = Hi.M(Hi.TailOther()) - Lo.M(Lo.TailOther());
			const bool bGroupsExplainIt =
				FMath::Abs(DTail) > 1e-9 && FMath::Abs(DGroup) > 0.50 * FMath::Abs(DTail);

			// THE NEGATIVE VERDICT NEEDS A PRECONDITION, and not having one is
			// how this line came to print GROUPS-DO-NOT-EXPLAIN-DTAIL for every
			// input ever fed to it: with zero scopes compiled in, DGroup was
			// identically 0 and the else branch was unreachable-by-arithmetic.
			// A verdict that cannot come out the other way is not a verdict.
			//
			// So a group total of zero across an armed level-2 window is now
			// reported as an INSTRUMENT state, not as a finding about streaming,
			// and any unwired group at all downgrades the negative verdict to
			// NOT-DECIDABLE. The POSITIVE verdict is unaffected: hits that were
			// actually recorded are evidence whatever else is missing.
			const int32 Unwired = TailGroupsUnwired();
			const double HitsHi = Hi.M(Hi.TailGroupHits());
			const double HitsLo = Lo.M(Lo.TailGroupHits());
			const bool bNoScopeRan = (HitsHi + HitsLo) <= 0.0;

			const TCHAR* TailVerdict =
				  bGroupsExplainIt ? TEXT("STREAMING-RENDER-COMMANDS-EXPLAIN-THE-DELTA")
				: bNoScopeRan      ? TEXT("NO-TAIL-SCOPE-RECORDED-A-SINGLE-HIT-THIS-IS-AN-INSTRUMENT-STATE-NOT-A-FINDING")
				: Unwired > 0      ? TEXT("NOT-DECIDABLE-GROUPS-ARE-UNWIRED-SEE-TAIL-WIRING")
				:                    TEXT("GROUPS-DO-NOT-EXPLAIN-DTAIL-SEE-dTailOther");

			UE_LOG(LogVoxelRenderFrame, Log,
			       TEXT("Voxel render frame DELTA-TAIL tag=%s VERDICT=%s sitesWired=%d ")
			       TEXT("groupsUnwired=%d groupHits/frame=%.1f(moving,n=%lld) %.1f(parked,n=%lld) dTailMs=%+.2f ")
			       TEXT("dGroupTotalMs=%+.2f (%.0f%% of dTail) dTailOtherMs=%+.2f (%.0f%% of dTail) ")
			       TEXT("| dMeshJobMs=%+.3f dBrickPoolMs=%+.3f dChunkIndexMs=%+.3f dResidencyMs=%+.3f ")
			       TEXT("dPoolCompMs=%+.3f dGiVolMs=%+.3f | dMeshJobHits=%+.1f dBrickPoolHits=%+.1f ")
			       TEXT("dResidencyHits=%+.1f dGiVolHits=%+.1f -- HITS ARE THE TRAFFIC COUNTER AND ")
			       TEXT("THEY COME FIRST: a group whose ms rose while its hit count did not is a group ")
			       TEXT("whose commands got MORE EXPENSIVE, and a group whose hits rose is one that ran ")
			       TEXT("MORE OFTEN; those are different fixes. If dTailOther carries most of dTail the ")
			       TEXT("29 instrumented sites are NOT the mechanism and no group here may be named as ")
			       TEXT("it -- BUT ONLY IF groupsUnwired=0. While groupsUnwired>0 this line cannot ")
			       TEXT("reach that conclusion at all: the missing scopes are meshJob and brickPool, ")
			       TEXT("the two groups most likely to carry it. sitesWired is the DENOMINATOR of ")
			       TEXT("every ms and hit figure on this line. win=%.2fs"),
			       Tag, TailVerdict, TailSitesWiredTotal(), Unwired,
			       HitsHi, (long long)Hi.N, HitsLo, (long long)Lo.N,
			       DTail, DGroup,
			       FMath::Abs(DTail) > 1e-9 ? 100.0 * DGroup / DTail : 0.0,
			       DOtherTail,
			       FMath::Abs(DTail) > 1e-9 ? 100.0 * DOtherTail / DTail : 0.0,
			       Hi.M(Hi.Sub[int32(EBucket::TailGpuMeshJob)])    - Lo.M(Lo.Sub[int32(EBucket::TailGpuMeshJob)]),
			       Hi.M(Hi.Sub[int32(EBucket::TailBrickPool)])     - Lo.M(Lo.Sub[int32(EBucket::TailBrickPool)]),
			       Hi.M(Hi.Sub[int32(EBucket::TailChunkIndex)])    - Lo.M(Lo.Sub[int32(EBucket::TailChunkIndex)]),
			       Hi.M(Hi.Sub[int32(EBucket::TailResidency)])     - Lo.M(Lo.Sub[int32(EBucket::TailResidency)]),
			       Hi.M(Hi.Sub[int32(EBucket::TailPoolComponent)]) - Lo.M(Lo.Sub[int32(EBucket::TailPoolComponent)]),
			       Hi.M(Hi.Sub[int32(EBucket::TailGIVolume)])      - Lo.M(Lo.Sub[int32(EBucket::TailGIVolume)]),
			       Hi.M(Hi.SubHits[int32(EBucket::TailGpuMeshJob)]) - Lo.M(Lo.SubHits[int32(EBucket::TailGpuMeshJob)]),
			       Hi.M(Hi.SubHits[int32(EBucket::TailBrickPool)])  - Lo.M(Lo.SubHits[int32(EBucket::TailBrickPool)]),
			       Hi.M(Hi.SubHits[int32(EBucket::TailResidency)])  - Lo.M(Lo.SubHits[int32(EBucket::TailResidency)]),
			       Hi.M(Hi.SubHits[int32(EBucket::TailGIVolume)])   - Lo.M(Lo.SubHits[int32(EBucket::TailGIVolume)]),
			       WindowSec);
		}
	}

	void Flush(double Now)
	{
		FState& S = Get();
		const double WindowSec = Now - S.WindowStartSeconds;

		EmitSegment(TEXT("SETTLED-MOVING"), S.MoveWindow, WindowSec);
		EmitSegment(TEXT("SETTLED-PARKED"), S.ParkWindow, WindowSec);
		EmitSegment(TEXT("FILL"),           S.FillWindow, WindowSec);
		EmitDelta(TEXT("moving-vs-parked-window"), S.MoveWindow, S.ParkWindow, WindowSec);

		const double LegSec = Now - S.LegStartSeconds;
		EmitSegment(TEXT("SETTLED-MOVING-LEG"), S.MoveTotal, LegSec);
		EmitSegment(TEXT("SETTLED-PARKED-LEG"), S.ParkTotal, LegSec);
		EmitDelta(TEXT("moving-vs-parked-LEG"), S.MoveTotal, S.ParkTotal, LegSec);

		S.MoveWindow.Reset();
		S.ParkWindow.Reset();
		S.FillWindow.Reset();
		S.WindowStartSeconds = Now;
		S.LastLogSeconds = Now;
	}

	// Close the frame that anchor A opened, now that the NEXT frame's anchor A
	// has arrived and the engine globals describe the frame just finished.
	void CloseFrame(FState& S, uint64 CyNextA, uint32 IdleNextA)
	{
		if (!S.bFrameOpen)
		{
			return;
		}
		S.bFrameOpen = false;

		if (!S.bHaveB || !S.bHaveE)
		{
			// A fired and the graph never finished executing on the path this
			// file assumes. Discarded rather than reconstructed -- a
			// reconstructed frame is a fabricated one.
			++S.Dropped;
			return;
		}

		const double PeriodMs = CyToMs(CyNextA - S.CyA);
		const double RenderBusyMs = Cy32ToMs(GRenderThreadTime);
		const double RenderWaitMs = Cy32ToMs(GRenderThreadWaitTime);
		const double RhiMs        = Cy32ToMs(GRHIThreadTime);

		// THE STARTUP GUARD, and it is the same one VoxelFramePhase needed: the
		// engine's per-thread timers are written at Present and carry whatever
		// accumulated before the first one. Frames 1 and 2 of a cold leg have
		// already produced one mechanism that was not there
		// (renderWait=9962 ms on a 400 ms frame, the same value twice).
		if (PeriodMs <= 0.0 || RenderBusyMs > 3.0 * PeriodMs || RenderWaitMs > 3.0 * PeriodMs)
		{
			++S.SkippedStartup;
			return;
		}

		// Idle per bucket, from the two counters the engine itself sums.
		//
		// Both counters are RESET at Present, so within a frame they are
		// absolute and a difference between two anchors is that span's idle.
		const double IdleSetup = Cy32ToMs(S.IdleB - S.IdleA);
		const double IdleExec  = Cy32ToMs(S.IdleE - S.IdleB);

		// THE TAIL SPANS PRESENT, where FThreadIdleStats is Reset() and the
		// GPUPresent idle is folded in, so its idle cannot be sampled as a
		// difference -- the counter goes backwards across the boundary. It is
		// reconstructed from the two halves either side:
		//
		//   [E .. Present]      = GRenderThreadWaitTime(N) - I(E)
		//   [Present .. A']     = I(A')            <- IdleNextA, and this is the
		//                                             reason the next frame's
		//                                             anchor passes its counter
		//                                             in rather than only its clock
		//
		// DELIBERATELY NOT CLAMPED AT ZERO. If the reconstruction ever goes
		// negative the tail bucket claims more busy time than it had, the
		// reconciliation delta grows, and the line says recon=INVALID. Clamping
		// would hide exactly the drift the delta exists to expose -- and a
		// bucket that cannot go negative is not a measurement.
		const double IdleTail =
			(RenderWaitMs - Cy32ToMs(S.IdleE)) + Cy32ToMs(IdleNextA);

		const double SetupWall   = CyToMs(S.CyB - S.CyA);
		const double ExecuteWall = CyToMs(S.CyE - S.CyB);
		const double TailWall    = CyToMs(CyNextA - S.CyE);

		FBucketSums Sample;
		Sample.N = 1;
		Sample.SetupBusy   = SetupWall   - IdleSetup;
		Sample.ExecuteBusy = ExecuteWall - IdleExec;
		Sample.TailBusy    = TailWall    - IdleTail;
		for (int32 i = 0; i < int32(EBucket::Num); ++i)
		{
			Sample.Sub[i] = S.SubThisFrame[i];
			Sample.SubHits[i] = S.SubHitsThisFrame[i];
		}
		for (int32 i = 0; i < 3; ++i)
		{
			Sample.PhaseBusy[i] = S.PhaseBusyThisFrame[i];
		}
		Sample.SveBlocked = S.SveBlockedThisFrame;
		Sample.PeriodWall = PeriodMs;
		Sample.RenderBusy = RenderBusyMs;
		Sample.RenderWait = RenderWaitMs;
		Sample.RhiBusy    = RhiMs;
		Sample.Families   = double(S.FamiliesThisFrame);
		Sample.Touches    = double(S.TouchesThisFrame);
		Sample.Views      = double(S.ViewsThisFrame);
		Sample.MarchTiles = double(S.MarchTilesThisFrame);

		// CAMERA SPEED, DERIVED ON THE RENDER THREAD, and the reason it is
		// derived rather than published: the publisher would be a hook in
		// VoxelWorldSubsystem.cpp, which this workstream may not edit, and the
		// camera is in any case the thing the RENDER frame responds to. It is
		// printed on every line so it can be checked against the leg's own
		// -VoxelPerfSpeed; if the two disagree the segmentation is wrong and
		// says so rather than quietly classifying a flight as parked.
		double SpeedUU = 0.0;
		if (S.bHaveViewOriginThisFrame && S.bHaveLastViewOrigin && PeriodMs > 0.0)
		{
			SpeedUU = (S.ViewOriginThisFrame - S.LastViewOrigin).Size() / (PeriodMs / 1000.0);
		}
		Sample.CamSpeedUU = SpeedUU;
		if (S.bHaveViewOriginThisFrame)
		{
			S.LastViewOrigin = S.ViewOriginThisFrame;
			S.bHaveLastViewOrigin = true;
		}

		// THE SETTLE LATCH, read from the streaming system's own verdict
		// (which applies voxel.March.SettleFrames, so the threshold is not
		// duplicated here). Latched rather than live: during a flight the
		// converged counter is driven back to zero by the very streaming the
		// flight causes, so a live read would classify the whole flight as
		// COLD FILL -- and folding the flight into the fill population is
		// exactly the substitution the segmentation exists to prevent.
		// Either witness latches it. NoteSettled() is authoritative and fires
		// once at the cold-settle site; the converged poll is kept as a
		// fallback for runs that never reach that site (a leg killed early),
		// but it is NOT sufficient on its own -- see NoteSettled()'s comment.
		if (GRenderFrameSettledLatch.load(std::memory_order_relaxed) || VoxelMarchIsStreamConverged())
		{
			S.bEverSettled = true;
		}

		const bool bMoving = SpeedUU > LatchedMoveThresholdUU();
		auto Add = [&Sample](FBucketSums& Dst)
		{
			Dst.N += Sample.N;
			Dst.SetupBusy += Sample.SetupBusy;
			Dst.ExecuteBusy += Sample.ExecuteBusy;
			Dst.TailBusy += Sample.TailBusy;
			for (int32 i = 0; i < int32(EBucket::Num); ++i)
			{
				Dst.Sub[i] += Sample.Sub[i];
				Dst.SubHits[i] += Sample.SubHits[i];
			}
			for (int32 i = 0; i < 3; ++i) { Dst.PhaseBusy[i] += Sample.PhaseBusy[i]; }
			Dst.SveBlocked += Sample.SveBlocked;
			Dst.PeriodWall += Sample.PeriodWall;
			Dst.RenderBusy += Sample.RenderBusy;
			Dst.RenderWait += Sample.RenderWait;
			Dst.RhiBusy += Sample.RhiBusy;
			Dst.CamSpeedUU += Sample.CamSpeedUU;
			Dst.Families += Sample.Families;
			Dst.Touches += Sample.Touches;
			Dst.Views += Sample.Views;
			Dst.MarchTiles += Sample.MarchTiles;
		};

		if (!S.bEverSettled)
		{
			Add(S.FillWindow);
			Add(S.FillTotal);
		}
		else if (bMoving)
		{
			Add(S.MoveWindow);
			Add(S.MoveTotal);
		}
		else
		{
			Add(S.ParkWindow);
			Add(S.ParkTotal);
		}

		++S.FramesSeen;
	}
} // namespace

int32 Mode()
{
	return LatchedMode();
}

int32 MutateArm()
{
	return LatchedMutateArm();
}

void Touch(FRDGBuilder& GraphBuilder)
{
	if (LatchedMode() == 0 || !IsInRenderingThread())
	{
		return;
	}
	FState& S = Get();
	const uint32 Frame = GFrameNumberRenderThread;
	const uint64 Cy = FPlatformTime::Cycles64();
	const uint32 Idle = SampleIdleCycles();
	// THE FAMILY KEY. Not the FSceneViewFamily -- Touch()'s signature does not
	// carry one and the two hooks that call it are in a file this change may
	// not edit -- but the RDG graph, which is the thing whose extra Execute is
	// what actually breaks the split. One graph per render node, one render
	// node per family (SceneRenderBuilder.cpp:873/:916).
	const void* const GraphKey = static_cast<const void*>(&GraphBuilder);

	if (S.bFrameOpen && S.OpenFrameNumber == Frame)
	{
		++S.TouchesThisFrame;

		if (GraphKey == S.OpenGraph)
		{
			// THE SAME GRAPH, A LATER HOOK. This is the ordinary case and it is
			// the whole defect: PreRenderView_RenderThread is a deliberate
			// idempotent safety anchor and the fluid extension takes the anchor
			// too, so a healthy one-family frame reaches here one to three
			// times. It must not count a family. It stays safe to call.
			return;
		}

		// A DIFFERENT GRAPH INSIDE THE SAME FRAME. Anchor E has already fired
		// for the previous one (that is what cleared OpenGraph), so an entire
		// Execute has now happened between anchor A and the anchor B that is
		// still to come: setupMs will swallow it and the three-way split is NOT
		// a partition. Counted, printed, and D0 fails on it.
		++S.FamiliesThisFrame;
		S.OpenGraph = GraphKey;
		// B AND E MOVE TO THE NEW GRAPH, which is what makes the printed
		// numbers match the header's own description of this case: setupMs then
		// runs A(family 1) -> B(last family) and SWALLOWS the intermediate
		// Execute. Leaving bHaveE set from the previous graph would instead
		// book this family's setup scopes into the TAIL phase and hide the
		// swallowing inside a bucket that is not supposed to contain it.
		S.bHaveB = false;
		S.bHaveE = false;
		RegisterAnchorE(GraphBuilder);
		return;
	}

	CloseFrame(S, Cy, Idle);

	S.bFrameOpen = true;
	S.OpenFrameNumber = Frame;
	S.CyA = Cy;
	S.IdleA = Idle;
	S.bHaveB = false;
	S.bHaveE = false;
	S.OpenGraph = GraphKey;
	// ONE GRAPH SO FAR. The fake-family arm adds synthetic ones so the gate can
	// be shown to fail on demand; at its default of 0 or 1 it adds nothing.
	S.FamiliesThisFrame = FMath::Max(1, LatchedFakeFamilies());
	S.TouchesThisFrame = 1;
	S.ViewsThisFrame = 0;
	S.MarchTilesThisFrame = 0;
	S.SveBlockedThisFrame = 0.0;
	S.bHaveViewOriginThisFrame = false;
	for (int32 i = 0; i < int32(EBucket::Num); ++i)
	{
		S.SubThisFrame[i] = 0.0;
		S.SubHitsThisFrame[i] = 0.0;
	}
	for (int32 i = 0; i < 3; ++i) { S.PhaseBusyThisFrame[i] = 0.0; }

	if (!S.bStarted)
	{
		S.bStarted = true;
		S.WindowStartSeconds = FPlatformTime::Seconds();
		S.LastLogSeconds = S.WindowStartSeconds;
		S.LegStartSeconds = S.WindowStartSeconds;
		S.ScopeCostNs = CalibrateScopeNs();
		UE_LOG(LogVoxelRenderFrame, Display,
		       TEXT("Voxel render frame ARMED (-VoxelRenderFrame=%d, mutate=%d %.2fms, fakeFamilies=%d, ")
		       TEXT("window=%.2fs ")
		       TEXT("from -VoxelPerfLogInterval, moveThresholdUU=%.0f). Anchors: A=PreRenderViewFamily, ")
		       TEXT("B=PostRenderViewFamily, E=RDG post-execute. Buckets are BUSY (idle subtracted from ")
		       TEXT("FThreadIdleStats::Waits + GRenderThreadIdle[GPUQuery]) and reconcile against ")
		       TEXT("GRenderThreadTime with the delta printed. If no 'Voxel render frame seg=' line ")
		       TEXT("follows this one, the hooks were applied but no frame ever closed. ")
		       TEXT("scopeCostNs=%.1f measured at arm time (level 2 pays this per render command; ")
		       TEXT("multiply by l2Hits on the TAIL line for the armed overhead, and if that is a ")
		       TEXT("meaningful share of tailMs the attribution arm is not a control for the D4 arm)."),
		       LatchedMode(), LatchedMutateArm(), LatchedMutateMs(), LatchedFakeFamilies(),
		       LatchedWindowSeconds(),
		       LatchedMoveThresholdUU(), S.ScopeCostNs);
	}

	// ANCHOR E. Registered on the graph that is about to be built, so it fires
	// at the end of THIS frame's Execute (RenderGraphBuilder.cpp:2214), after
	// the parallel-translate await.
	RegisterAnchorE(GraphBuilder);

	const double Now = FPlatformTime::Seconds();
	if (Now - S.LastLogSeconds >= LatchedWindowSeconds())
	{
		Flush(Now);
	}
}

void NoteSetupEnd()
{
	if (LatchedMode() == 0 || !IsInRenderingThread())
	{
		return;
	}
	FState& S = Get();
	if (!S.bFrameOpen)
	{
		return;
	}
	// LAST FAMILY WINS. With one family this is the only call.
	S.CyB = FPlatformTime::Cycles64();
	S.IdleB = SampleIdleCycles();
	S.bHaveB = true;
}

void NoteView(const FVector& ViewOriginUU)
{
	if (LatchedMode() == 0 || !IsInRenderingThread())
	{
		return;
	}
	FState& S = Get();
	if (!S.bFrameOpen)
	{
		return;
	}
	++S.ViewsThisFrame;
	if (!S.bHaveViewOriginThisFrame)
	{
		S.ViewOriginThisFrame = ViewOriginUU;
		S.bHaveViewOriginThisFrame = true;
	}
}

void NoteMarchTiles(uint32 Tiles)
{
	if (LatchedMode() == 0 || !IsInRenderingThread())
	{
		return;
	}
	FState& S = Get();
	if (S.bFrameOpen)
	{
		S.MarchTilesThisFrame += Tiles;
	}
}

FScope::FScope(EBucket InBucket, int32 InMinMode)
	: Bucket(InBucket)
{
	if (LatchedMode() < InMinMode || !IsInRenderingThread())
	{
		return;
	}
	bArmed = true;
	StartCycles = FPlatformTime::Cycles64();
	StartIdle = SampleIdleCycles();
}

FScope::~FScope()
{
	if (!bArmed)
	{
		return;
	}
	const double WallMs = CyToMs(FPlatformTime::Cycles64() - StartCycles);
	const double IdleMs = Cy32ToMs(SampleIdleCycles() - StartIdle);
	FState& S = Get();
	if (!S.bFrameOpen)
	{
		// A render command that ran with no frame open -- before the first scene
		// render, or after the last. Counted nowhere rather than banked into an
		// arbitrary frame; the frame's own residual is what would absorb it, and
		// absorbing work that did not happen in a frame is how a residual stops
		// meaning anything.
		return;
	}

	// WHICH HALF OF THE FRAME THIS CLOSED IN, decided by the anchors that have
	// already fired rather than by an assumption about where render commands
	// run. A tail-group scope that ever closed inside the scene renderer would
	// be booked to setup and would show up as setupOther shrinking -- visible,
	// rather than silently inflating tail.
	const int32 Phase = !S.bHaveB ? 0 : (!S.bHaveE ? 1 : 2);

	const double BusyMs = WallMs - IdleMs;
	S.SubThisFrame[int32(Bucket)] += BusyMs;
	S.SubHitsThisFrame[int32(Bucket)] += 1.0;
	S.PhaseBusyThisFrame[Phase] += BusyMs;
	S.SveBlockedThisFrame += IdleMs;
}

void MutateHere(int32 ArmId)
{
	if (LatchedMode() == 0 || LatchedMutateArm() != ArmId)
	{
		return;
	}
	if (ArmId == 3)
	{
		// BLOCKING, not burning. This is the arm that proves the idle
		// correction: it must show up in sveBlocked and renderWait and must NOT
		// move mBase busy or renderBusy. FEventRef is what the engine's own
		// idle accounting sees; a raw Sleep would not be counted as a wait and
		// the arm would prove the opposite of what it claims.
		UE::Stats::FThreadIdleStats::FScopeIdle IdleScope;
		FPlatformProcess::Sleep(float(LatchedMutateMs() / 1000.0));
		return;
	}
	BurnMs(LatchedMutateMs());
}
} // namespace VoxelRenderFrame

namespace VoxelRenderFrame
{
void NoteSettled()
{
	GRenderFrameSettledLatch.store(true, std::memory_order_relaxed);
}
} // namespace VoxelRenderFrame
