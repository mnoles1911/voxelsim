// VoxelFramePhase.cpp -- see VoxelFramePhase.h for the falsified lead, the
// reconciliation this adds that the existing attribution cannot express, the
// registered disproof for all three hypotheses, and the failing readings.

#include "VoxelFramePhase.h"

#include "VoxelDebug.h"  // LogVoxelPerf
#include "RenderTimer.h" // GRenderThreadTime / GRenderThreadWaitTime / GRHIThreadTime / GGameThreadTime / GGameThreadWaitTime

namespace VoxelFramePhase
{
namespace
{

int32 HeavyThreshold()
{
	static const int32 Latched = []
	{
		// 512: half of the -VoxelApplyCap=1024 regime the question is about, so
		// a heavy frame is unambiguously a lifted-cap frame and not a default-cap
		// frame that happened to run long. The default-cap ceiling is 192, so no
		// 192-capped frame can ever land in the heavy bucket by accident -- which
		// is what keeps the two populations from blurring into each other.
		int32 Value = 512;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelFramePhaseHeavy="), Value);
		return FMath::Max(1, Value);
	}();
	return Latched;
}

// LIGHT is a fixed 64 rather than a fraction of HEAVY: it has to mean "this
// frame did almost no applying", and tying it to the heavy threshold would let
// a sweep of -VoxelFramePhaseHeavy silently redefine the baseline the delta is
// measured against. A delta whose BOTH ends move is not a delta.
constexpr int32 kLightThreshold = 64;

struct FBucket
{
	double Frame = 0, Tick = 0, GameThread = 0, GameWait = 0;
	double Render = 0, RenderWait = 0, RHI = 0;
	int64 Applies = 0;
	int32 N = 0;

	void Add(double InFrame, double InTick, double InGame, double InGameWait,
	         double InRender, double InRenderWait, double InRHI, int32 InApplies)
	{
		Frame += InFrame; Tick += InTick; GameThread += InGame; GameWait += InGameWait;
		Render += InRender; RenderWait += InRenderWait; RHI += InRHI;
		Applies += InApplies; ++N;
	}
	double M(double V) const { return N > 0 ? V / double(N) : 0.0; }
	// gameOther = everything the game thread did that was NOT the streaming
	// tick. Clamped at 0: GGameThreadTime lags frameMs by one frame (it is set
	// in FViewport::Draw), so on a frame where the tick spiked it can come out
	// slightly below the tick and a negative "other" would be an artifact of the
	// lag, not work that un-happened. The clamp pushes that error into the
	// residual, where it is visible, instead of into a named bucket, where it
	// would not be.
	double MOther() const { return FMath::Max(0.0, M(GameThread) - M(Tick)); }
	double MResidual() const { return M(Frame) - M(Tick) - MOther() - M(GameWait); }
};

FBucket All, Heavy, Light;
double LastFrameSeconds = 0.0;
double LastLogSeconds = 0.0;
int32 PeakAppliesThisWindow = 0;
int64 ZeroGameThreadFrames = 0; // hard-zero guard; see the header
int64 NegativeResidualFrames = 0;

void Emit(const TCHAR* Name, const FBucket& B)
{
	if (B.N == 0)
	{
		UE_LOG(LogVoxelPerf, Log, TEXT("Voxel frame phase %s: n=0 -- THIS POPULATION IS EMPTY. ")
		                          TEXT("Nothing below describes it and no verdict may be drawn from it."), Name);
		return;
	}
	const double F = B.M(B.Frame);
	// SHARES ARE PRINTED BESIDE THEIR OWN ABSOLUTE, ALWAYS. waitShare=99% was
	// true and meaningless tonight; a share with no millisecond next to it is
	// not a measurement on this project any more.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel frame phase %s: n=%d appl/frame=%.1f | frame=%.2fms = tick %.2f (%.0f%%) ")
	       TEXT("+ gameOther %.2f (%.0f%%) + gameWait %.2f (%.0f%%) + RESIDUAL %.2f (%.0f%%) ")
	       TEXT("|| render=%.2f (%.0f%% of frame) renderWait=%.2f rhi=%.2f"),
	       Name, B.N, B.N > 0 ? double(B.Applies) / double(B.N) : 0.0,
	       F,
	       B.M(B.Tick),    F > 0 ? 100.0 * B.M(B.Tick) / F : 0.0,
	       B.MOther(),     F > 0 ? 100.0 * B.MOther() / F : 0.0,
	       B.M(B.GameWait), F > 0 ? 100.0 * B.M(B.GameWait) / F : 0.0,
	       B.MResidual(),  F > 0 ? 100.0 * B.MResidual() / F : 0.0,
	       B.M(B.Render),  F > 0 ? 100.0 * B.M(B.Render) / F : 0.0,
	       B.M(B.RenderWait), B.M(B.RHI));
}

void Flush(double Now)
{
	const double WindowSec = Now - LastLogSeconds;

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel frame phase (%.1fs window): heavy>=%d light<%d peakAppl/frame=%d ")
	       TEXT("| zeroGameThreadFrames=%lld negResidualFrames=%lld"),
	       WindowSec, HeavyThreshold(), kLightThreshold, PeakAppliesThisWindow,
	       (long long)ZeroGameThreadFrames, (long long)NegativeResidualFrames);
	Emit(TEXT("ALL  "), All);
	Emit(TEXT("LIGHT"), Light);
	Emit(TEXT("HEAVY"), Heavy);

	// THE DECIDING LINE, and it is allowed to name nothing.
	//
	// Read it against the disproof registered in the header BEFORE this leg:
	//   H1 render-thread    disproved if dRender < 20% AND dGameWait < 20%
	//   H2 game-thread      disproved if dTick + dOther < 50%
	//   H3 unattributed     confirmed  if dResidual > 50%
	//
	// Both H1 and H2 can fail at once. That is a result, not a gap to be filled
	// by preference.
	if (Heavy.N > 0 && Light.N > 0)
	{
		const double DFrame = Heavy.M(Heavy.Frame) - Light.M(Light.Frame);
		const double DTick = Heavy.M(Heavy.Tick) - Light.M(Light.Tick);
		const double DOther = Heavy.MOther() - Light.MOther();
		const double DWait = Heavy.M(Heavy.GameWait) - Light.M(Light.GameWait);
		const double DRes = Heavy.MResidual() - Light.MResidual();
		const double DRender = Heavy.M(Heavy.Render) - Light.M(Light.Render);
		const double DRenderWait = Heavy.M(Heavy.RenderWait) - Light.M(Light.RenderWait);
		const double DRHI = Heavy.M(Heavy.RHI) - Light.M(Light.RHI);
		auto Share = [DFrame](double V) { return FMath::Abs(DFrame) > 1e-6 ? 100.0 * V / DFrame : 0.0; };

		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame phase DELTA (heavy-light): frame=%+.2fms | tick=%+.2f (%.0f%%) ")
		       TEXT("gameOther=%+.2f (%.0f%%) gameWait=%+.2f (%.0f%%) RESIDUAL=%+.2f (%.0f%%) ")
		       TEXT("|| render=%+.2f (%.0f%%) renderWait=%+.2f rhi=%+.2f"),
		       DFrame, DTick, Share(DTick), DOther, Share(DOther), DWait, Share(DWait),
		       DRes, Share(DRes), DRender, Share(DRender), DRenderWait, DRHI);

		// The verdict, stated by the instrument so a reader cannot quietly pick
		// the answer they came for. Deliberately reports "BOTH DISPROVED" as a
		// first-class outcome.
		const bool bH1Disproved = FMath::Abs(Share(DRender)) < 20.0 && FMath::Abs(Share(DWait)) < 20.0;
		const bool bH2Disproved = FMath::Abs(Share(DTick) + Share(DOther)) < 50.0;
		const bool bH3 = Share(DRes) > 50.0;
		const TCHAR* Verdict =
			FMath::Abs(DFrame) < 1.0
				? TEXT("NO FRAME-TIME DIFFERENCE -- apply volume does not move frame time; the ceiling is not visible from here")
			: bH3
				? TEXT("H3 UNATTRIBUTED -- the residual carries it; NOT the render thread, NOT the streaming tick")
			: (bH1Disproved && bH2Disproved)
				? TEXT("H1 AND H2 BOTH DISPROVED -- neither thread's named work explains it; read the residual")
			: bH1Disproved
				? TEXT("H1 DISPROVED -- NOT the render thread")
			: bH2Disproved
				? TEXT("H2 DISPROVED -- NOT game-thread work")
				: TEXT("NEITHER HYPOTHESIS DISPROVED -- both threads moved; this instrument cannot separate them");
		UE_LOG(LogVoxelPerf, Log, TEXT("Voxel frame phase VERDICT: %s"), Verdict);
	}
	else
	{
		// heavyN=0 is the "read the peak window" guard from the header: a window
		// that never applied heavily says NOTHING about the lifted-cap regime.
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame phase VERDICT: NOT COMPUTED -- heavyN=%d lightN=%d. A window without both ")
		       TEXT("populations describes only the regime it was in; it is not evidence about the other."),
		       Heavy.N, Light.N);
	}

	All = Heavy = Light = FBucket{};
	PeakAppliesThisWindow = 0;
	ZeroGameThreadFrames = 0;
	NegativeResidualFrames = 0;
	LastLogSeconds = Now;
}

} // namespace

void NoteFrameImpl(double VoxelTickMs, int32 AppliesThisFrame)
{
	const double Now = FPlatformTime::Seconds();
	if (LastFrameSeconds <= 0.0)
	{
		// First call: no previous frame to difference against, and the engine
		// globals still hold their startup accumulation -- which is exactly the
		// 9,962 ms artifact this file opens with. Start the clocks, record
		// nothing.
		LastFrameSeconds = Now;
		LastLogSeconds = Now;
		return;
	}

	const double FrameMs = (Now - LastFrameSeconds) * 1000.0;
	LastFrameSeconds = Now;

	const double CyToMs = FPlatformTime::GetSecondsPerCycle() * 1000.0;
	const double RenderMs     = double(GRenderThreadTime) * CyToMs;
	const double RenderWaitMs = double(GRenderThreadWaitTime) * CyToMs;
	const double RHIMs        = double(GRHIThreadTime) * CyToMs;
	const double GameMs       = double(GGameThreadTime) * CyToMs;
	const double GameWaitMs   = double(GGameThreadWaitTime) * CyToMs;

	// THE STARTUP GUARD, GENERALISED. The engine globals are set in
	// FViewport::Draw and carry whatever accumulated before the first draw.
	// Any frame where a per-thread timer exceeds several times its own frame is
	// not describing that frame; it is describing history. Two such frames
	// exist in ahead-on.log and they are frames 1 and 2 -- and one of them is
	// the entire reason this investigation was commissioned.
	if (RenderMs > 3.0 * FrameMs || RenderWaitMs > 3.0 * FrameMs || GameMs > 3.0 * FrameMs)
	{
		return;
	}

	if (GameMs <= 0.0)
	{
		++ZeroGameThreadFrames;
	}

	PeakAppliesThisWindow = FMath::Max(PeakAppliesThisWindow, AppliesThisFrame);

	All.Add(FrameMs, VoxelTickMs, GameMs, GameWaitMs, RenderMs, RenderWaitMs, RHIMs, AppliesThisFrame);
	if (AppliesThisFrame >= HeavyThreshold())
	{
		Heavy.Add(FrameMs, VoxelTickMs, GameMs, GameWaitMs, RenderMs, RenderWaitMs, RHIMs, AppliesThisFrame);
	}
	else if (AppliesThisFrame < kLightThreshold)
	{
		Light.Add(FrameMs, VoxelTickMs, GameMs, GameWaitMs, RenderMs, RenderWaitMs, RHIMs, AppliesThisFrame);
	}

	if (FrameMs - VoxelTickMs - FMath::Max(0.0, GameMs - VoxelTickMs) - GameWaitMs < -1.0)
	{
		++NegativeResidualFrames;
	}

	if (Now - LastLogSeconds >= 5.0)
	{
		Flush(Now);
	}
}

} // namespace VoxelFramePhase
