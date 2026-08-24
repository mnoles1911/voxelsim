// VoxelRenderFrame.cpp -- the render-frame split. See VoxelRenderFrame.h for
// the whole argument: what renderBusy is, where the three anchors come from,
// the registered disproof, the mutation arms and the failing readings.
//
// This file contains no policy. It measures, it reconciles, and it prints the
// reconciliation delta so that a drifting instrument is loud rather than
// silent.

#include "VoxelRenderFrame.h"

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
		double SveBlocked = 0.0;

		double PeriodWall = 0.0;
		double RenderBusy = 0.0;
		double RenderWait = 0.0;
		double RhiBusy = 0.0;

		double CamSpeedUU = 0.0;
		double Families = 0.0;
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

		// setupOther CAN COME OUT NEGATIVE and is deliberately not clamped. A
		// bucket that cannot go negative is not a measurement: if the named
		// sub-scopes ever exceed the A->B span the split is wrong, and the only
		// way a reader finds that out is by seeing a minus sign.
		double SetupOther() const { return SetupBusy - SubTotal(); }

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
		int32  ViewsThisFrame = 0;
		uint32 MarchTilesThisFrame = 0;
		double SubThisFrame[int32(EBucket::Num)] = {};
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

		// TRAFFIC BEFORE TIMING. If these do not move between parked and moving
		// then no timing difference between the two has a mechanism, and any
		// bucket delta is asking to be explained by something this file cannot
		// see.
		FState& S = Get();
		UE_LOG(LogVoxelRenderFrame, Log,
		       TEXT("Voxel render frame seg=%s TRAFFIC families/frame=%.2f%s views/frame=%.2f ")
		       TEXT("marchTiles/frame=%.0f camSpeedMS=%.1f framesTotal=%lld dropped=%lld skippedStartup=%lld ")
		       TEXT("settleLatched=%s -- families/frame above 1.01 means setupMs swallowed an ")
		       TEXT("intermediate Execute and the three-way split is NOT a partition; camSpeedMS ")
		       TEXT("near zero on a MOVING line means the segmenter is wrong and the leg is invalid, ")
		       TEXT("not fast. win=%.2fs"),
		       Name, FamPerFrame, bMultiFamily ? TEXT(" MULTI-FAMILY-SPLIT-NOT-A-PARTITION") : TEXT(""),
		       B.M(B.Views), B.M(B.MarchTiles), B.M(B.CamSpeedUU) / 100.0,
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
		const bool bD0 = (HiBusy > 1e-9) && (FMath::Abs(HiRecon) <= 0.15 * HiBusy)
		               && (Hi.M(Hi.Families) <= 1.01);

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
		       TEXT("| D0=%s D1=%s D2=%s D3=%s D4=%s -- thresholds are the ones registered in ")
		       TEXT("VoxelRenderFrame.h BEFORE this leg ran; D1 was expected to be DISPROVED. ")
		       TEXT("All four disproved at once is a legitimate result and must be reported as ")
		       TEXT("such, not resolved by picking the largest bucket. win=%.2fs"),
		       Tag, Verdict, (long long)Hi.N, (long long)Lo.N, DRender,
		       DSetup, Share(DSetup), DExec, Share(DExec), DTail, Share(DTail),
		       DMarcher, Share(DMarcher), DOther, Share(DOther),
		       bD0 ? TEXT("ok") : TEXT("FAILED"),
		       bD1 ? TEXT("held") : TEXT("disproved"),
		       bD2 ? TEXT("held") : TEXT("disproved"),
		       bD3 ? TEXT("held") : TEXT("disproved"),
		       bD4 ? TEXT("CONFIRMED") : TEXT("not-confirmed"),
		       WindowSec);
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
		}
		Sample.SveBlocked = S.SveBlockedThisFrame;
		Sample.PeriodWall = PeriodMs;
		Sample.RenderBusy = RenderBusyMs;
		Sample.RenderWait = RenderWaitMs;
		Sample.RhiBusy    = RhiMs;
		Sample.Families   = double(S.FamiliesThisFrame);
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
		if (VoxelMarchIsStreamConverged())
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
			for (int32 i = 0; i < int32(EBucket::Num); ++i) { Dst.Sub[i] += Sample.Sub[i]; }
			Dst.SveBlocked += Sample.SveBlocked;
			Dst.PeriodWall += Sample.PeriodWall;
			Dst.RenderBusy += Sample.RenderBusy;
			Dst.RenderWait += Sample.RenderWait;
			Dst.RhiBusy += Sample.RhiBusy;
			Dst.CamSpeedUU += Sample.CamSpeedUU;
			Dst.Families += Sample.Families;
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

	if (S.bFrameOpen && S.OpenFrameNumber == Frame)
	{
		// A LATER FAMILY OF THE SAME FRAME. Counted, not anchored -- and the
		// count is printed, because with more than one family the A->B span
		// swallows an entire intermediate Execute and the three-way split
		// stops being a partition.
		++S.FamiliesThisFrame;
		return;
	}

	CloseFrame(S, Cy, Idle);

	S.bFrameOpen = true;
	S.OpenFrameNumber = Frame;
	S.CyA = Cy;
	S.IdleA = Idle;
	S.bHaveB = false;
	S.bHaveE = false;
	S.FamiliesThisFrame = 1;
	S.ViewsThisFrame = 0;
	S.MarchTilesThisFrame = 0;
	S.SveBlockedThisFrame = 0.0;
	S.bHaveViewOriginThisFrame = false;
	for (int32 i = 0; i < int32(EBucket::Num); ++i) { S.SubThisFrame[i] = 0.0; }

	if (!S.bStarted)
	{
		S.bStarted = true;
		S.WindowStartSeconds = FPlatformTime::Seconds();
		S.LastLogSeconds = S.WindowStartSeconds;
		S.LegStartSeconds = S.WindowStartSeconds;
		UE_LOG(LogVoxelRenderFrame, Display,
		       TEXT("Voxel render frame ARMED (-VoxelRenderFrame=%d, mutate=%d %.2fms, window=%.2fs ")
		       TEXT("from -VoxelPerfLogInterval, moveThresholdUU=%.0f). Anchors: A=PreRenderViewFamily, ")
		       TEXT("B=PostRenderViewFamily, E=RDG post-execute. Buckets are BUSY (idle subtracted from ")
		       TEXT("FThreadIdleStats::Waits + GRenderThreadIdle[GPUQuery]) and reconcile against ")
		       TEXT("GRenderThreadTime with the delta printed. If no 'Voxel render frame seg=' line ")
		       TEXT("follows this one, the hooks were applied but no frame ever closed."),
		       LatchedMode(), LatchedMutateArm(), LatchedMutateMs(), LatchedWindowSeconds(),
		       LatchedMoveThresholdUU());
	}

	// ANCHOR E. Registered on the graph that is about to be built, so it fires
	// at the end of THIS frame's Execute (RenderGraphBuilder.cpp:2214), after
	// the parallel-translate await.
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
	});

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

FScope::FScope(EBucket InBucket)
	: Bucket(InBucket)
{
	if (LatchedMode() == 0 || !IsInRenderingThread())
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
		return;
	}
	S.SubThisFrame[int32(Bucket)] += (WallMs - IdleMs);
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
