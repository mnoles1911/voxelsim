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

// ---------------------------------------------------------------------------
// GOAL 3: THE SETTLED-ONLY FRAME DISTRIBUTION
// ---------------------------------------------------------------------------
//
// The only frame statistic in the tree is cumulative over the whole leg:
//
//   VoxelPerfRun post-warmup (t>=10s): frames=19425 p50=9.84ms p95=30.62ms
//                                      max=115.28ms hitches=564
//
// On q-repro-main that window opens at t=10 s and the world SETTLES at t=21.3 s,
// so eleven seconds of cold-fill storm -- the regime where hitches are
// explicitly authorised -- are averaged into the same p95 as the 270 s of
// settled play the >100 FPS goal is actually about. The two regimes have
// opposite rules now (fill: optimise settle seconds, hitches tolerated;
// settled: optimise p95, >100 fps means p95 < 10 ms), and one statistic cannot
// serve both. Nothing here can be judged until they are separated.
//
// A HISTOGRAM, NOT A SAMPLE BUFFER, so a 300 s leg and a 3,000 s leg cost the
// same 4.4 KB per segment and no leg length can silently start dropping
// samples. The bin width is printed beside every percentile: p95 is compared
// against a 10.00 ms gate, so a 0.10 ms bin below 32 ms is 1% at the gate, and
// a reader must be able to see that rather than assume exactness. The MAX is
// tracked exactly, outside the histogram, because the max is the one value a
// bin cannot approximate usefully.
constexpr int32 kFineBins   = 320;   // 0.10 ms each, 0 .. 32 ms
constexpr double kFineMs    = 0.10;
constexpr int32 kCoarseBins = 224;   // 1.00 ms each, 32 .. 256 ms
constexpr double kCoarseMs  = 1.00;
constexpr int32 kOverflowBin = kFineBins + kCoarseBins; // >= 256 ms
constexpr int32 kNumBins    = kOverflowBin + 1;

// The >100 FPS gate, and the hitch bar, as named constants so the log can print
// what it is judging against instead of leaving a reader to remember.
constexpr double kFpsGateMs = 10.0;   // 100 fps
constexpr double kHitchMs   = 33.3;   // the project's existing hitch bar

struct FDist
{
	int64 Bins[kNumBins] = {};
	int64 N = 0;
	int64 Hitches = 0;
	double MaxMs = 0.0;
	double SumMs = 0.0;

	void Add(double Ms)
	{
		++N;
		SumMs += Ms;
		MaxMs = FMath::Max(MaxMs, Ms);
		if (Ms >= kHitchMs) { ++Hitches; }
		int32 Bin;
		if (Ms < 32.0)       { Bin = FMath::Clamp(int32(Ms / kFineMs), 0, kFineBins - 1); }
		else if (Ms < 256.0) { Bin = kFineBins + FMath::Clamp(int32((Ms - 32.0) / kCoarseMs), 0, kCoarseBins - 1); }
		else                 { Bin = kOverflowBin; }
		++Bins[Bin];
	}

	// Upper edge of the bin the requested quantile falls in. UPPER, not centre:
	// a gate is a "must be under" test, so the pessimistic edge is the honest
	// one to compare against 10.00 ms. Stated here rather than left implicit.
	double Quantile(double Q) const
	{
		if (N <= 0) { return 0.0; }
		const int64 Target = int64(double(N) * Q);
		int64 Seen = 0;
		for (int32 I = 0; I < kNumBins; ++I)
		{
			Seen += Bins[I];
			if (Seen > Target)
			{
				if (I < kFineBins)       { return double(I + 1) * kFineMs; }
				if (I < kOverflowBin)    { return 32.0 + double(I - kFineBins + 1) * kCoarseMs; }
				return MaxMs; // overflow bin: the exact max is the only honest answer
			}
		}
		return MaxMs;
	}
	double Mean() const { return N > 0 ? SumMs / double(N) : 0.0; }
};

// Two segments, each with a per-window view and a cumulative view.
//
// CUMULATIVE IS WHAT THE GOAL IS JUDGED ON, and it is printed on every window
// precisely so that `grep | tail -1` -- which has produced four retractions on
// this project by landing on the post-flight linger -- cannot pick up a window
// with three frames in it and call it the answer. A cumulative row is monotone
// and carries its own n.
FDist FillWindow, FillTotal, SettledWindow, SettledTotal;
bool bSettled = false;
double SettleSeconds = -1.0;

const TCHAR* SegName() { return bSettled ? TEXT("SETTLED") : TEXT("FILL   "); }

void EmitDist(const TCHAR* Seg, const TCHAR* Scope, const FDist& D)
{
	if (D.N == 0)
	{
		// A hard, loud zero. On the SETTLED row this means the leg never settled
		// (or hook 2 was not applied), and the ONLY correct reading is "this leg
		// cannot speak to GOAL 3" -- never the fill numbers under a settled
		// heading, which is the exact blend the segmentation exists to undo.
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame dist %s %s: n=0 -- NO FRAMES IN THIS SEGMENT. ")
		       TEXT("If this is SETTLED, the leg never reached settle or hook 2 was not applied, ")
		       TEXT("and NO >100 FPS claim may be made from this leg either way."), Seg, Scope);
		return;
	}
	const double P50 = D.Quantile(0.50);
	const double P95 = D.Quantile(0.95);
	const double P99 = D.Quantile(0.99);
	// FPS printed beside every ms so nobody has to divide, and the gate verdict
	// stated rather than left to the reader -- but ONLY on the settled segment,
	// because the fill is explicitly exempt from it by the owner's standing
	// directive.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel frame dist %s %s: n=%lld mean=%.2fms | p50=%.2fms (%.0f fps) p95=%.2fms (%.0f fps) ")
	       TEXT("p99=%.2fms (%.0f fps) max=%.2fms | hitches=%lld (>%.1fms, %.2f%%) | bin=%.2f/%.2fms | %s"),
	       Seg, Scope, (long long)D.N, D.Mean(),
	       P50, P50 > 0 ? 1000.0 / P50 : 0.0,
	       P95, P95 > 0 ? 1000.0 / P95 : 0.0,
	       P99, P99 > 0 ? 1000.0 / P99 : 0.0,
	       D.MaxMs, (long long)D.Hitches, kHitchMs, 100.0 * double(D.Hitches) / double(D.N),
	       kFineMs, kCoarseMs,
	       bSettled && FCString::Strcmp(Seg, TEXT("SETTLED")) == 0
	           ? (P95 < kFpsGateMs ? TEXT("GOAL3 PASS (p95 < 10.00ms)") : TEXT("GOAL3 FAIL (p95 >= 10.00ms)"))
	           : TEXT("gate n/a -- fill is exempt (hitches authorised during the load storm)"));
}

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

	if (Mode() & kModeDistribution)
	{
		// THE WINDOW ROW SAYS WHICH REGIME IT DESCRIBES, in its own segment tag,
		// and the CUMULATIVE row beside it is what the goal is judged on.
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame dist (%.1fs window): segment=%s settleT=%s | gate: p95 < %.2fms == 100 fps"),
		       WindowSec, SegName(),
		       SettleSeconds >= 0.0 ? *FString::Printf(TEXT("%.1fs"), SettleSeconds) : TEXT("NOT SETTLED"),
		       kFpsGateMs);
		EmitDist(bSettled ? TEXT("SETTLED") : TEXT("FILL"), TEXT("window"),
		         bSettled ? SettledWindow : FillWindow);
		EmitDist(TEXT("FILL"),    TEXT("total "), FillTotal);
		EmitDist(TEXT("SETTLED"), TEXT("total "), SettledTotal);
		FillWindow = SettledWindow = FDist{};
	}

	if ((Mode() & kModeReconcile) == 0)
	{
		All = Heavy = Light = FBucket{};
		PeakAppliesThisWindow = 0;
		ZeroGameThreadFrames = 0;
		NegativeResidualFrames = 0;
		LastLogSeconds = Now;
		return;
	}

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

	// THE DISTRIBUTION TAKES EVERY FRAME, AND IT IS FED HERE -- ABOVE THE
	// STALE-GLOBALS GUARD BELOW -- ON PURPOSE.
	//
	// That guard rejects frames whose ENGINE GLOBALS are describing history
	// rather than this frame. The frame TIME is not one of those globals: it
	// came from this function's own clock two lines up and is sound whatever
	// the render timers say. Dropping a real 400 ms frame out of the tail
	// statistic because an unrelated global was stale would corrupt the exact
	// number GOAL 3 is judged on, in the conservative-looking direction, which
	// is the worst direction for a gate.
	if (Mode() & kModeDistribution)
	{
		(bSettled ? SettledWindow : FillWindow).Add(FrameMs);
		(bSettled ? SettledTotal  : FillTotal ).Add(FrameMs);
	}

	if ((Mode() & kModeReconcile) == 0)
	{
		if (Now - LastLogSeconds >= 5.0) { Flush(Now); }
		return;
	}

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
	//
	// RECONCILIATION ONLY. The distribution above has already banked this
	// frame's time.
	if (RenderMs > 3.0 * FrameMs || RenderWaitMs > 3.0 * FrameMs || GameMs > 3.0 * FrameMs)
	{
		if (Now - LastLogSeconds >= 5.0) { Flush(Now); }
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

void NoteSettledImpl(double InSettleSeconds)
{
	if (bSettled)
	{
		return; // the cold-settle line fires once; a second call is not a second boundary
	}
	bSettled = true;
	SettleSeconds = InSettleSeconds;
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel frame dist: SEGMENT BOUNDARY at t=%.1fs -- every frame from here is SETTLED. ")
	       TEXT("Fill so far: n=%lld p95=%.2fms max=%.2fms hitches=%lld (fill is exempt from the ")
	       TEXT("100 fps gate; everything after this line is not)."),
	       InSettleSeconds, (long long)FillTotal.N, FillTotal.Quantile(0.95),
	       FillTotal.MaxMs, (long long)FillTotal.Hitches);
}

} // namespace VoxelFramePhase
