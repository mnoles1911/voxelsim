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
constexpr double kFpsGateMs = 10.0;   // 100 fps -- part 1 of the gate
constexpr double kHitchMs   = 33.3;   // the project's EXISTING hitch bar, kept for continuity
// PART 2 OF THE GATE NEEDS ITS OWN BAR, and 33.3 ms is the wrong one for it.
//
// "Steady and above 100 FPS" (owner, 2026-08-23) is two requirements, and the
// second lives in the tail beyond p95: a 10 ms p95 with a 200 ms max is a
// failure the player feels. But at a 100 fps target a STUTTER is not a 33.3 ms
// frame -- 33.3 ms is the 30 fps bar, inherited from when that was the target.
// A frame at twice the gate is a dropped frame at 100 fps and is visible.
//
// So both are counted: `hitches` at the legacy 33.3 ms so every historical leg
// stays comparable, and `stutters` at 2x the gate, which is what part 2 is
// actually judged on. Two bars, two counters, neither renamed.
constexpr double kStutterMs = 2.0 * kFpsGateMs; // 20.00 ms = a dropped frame at 100 fps

struct FDist
{
	int64 Bins[kNumBins] = {};
	int64 N = 0;
	int64 Hitches = 0;   // >= 33.3 ms, the legacy bar
	int64 Stutters = 0;  // >= 2x the gate, what "steady" is judged on
	double MaxMs = 0.0;
	double SumMs = 0.0;
	// THE SEGMENT'S OWN SPEED, so a reader can confirm the MOVING rows were
	// taken at or above the 20 m/s the gate specifies. A MOVING segment
	// averaging 4 m/s does not test the gate, and without this the row would
	// look identical to one that does.
	double SumSpeedMps = 0.0;
	double MaxSpeedMps = 0.0;

	void Add(double Ms, double SpeedUU)
	{
		++N;
		SumMs += Ms;
		MaxMs = FMath::Max(MaxMs, Ms);
		const double Mps = SpeedUU / 100.0; // 100 UU = 1 m
		SumSpeedMps += Mps;
		MaxSpeedMps = FMath::Max(MaxSpeedMps, Mps);
		if (Ms >= kHitchMs)   { ++Hitches; }
		if (Ms >= kStutterMs) { ++Stutters; }
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
	double MeanSpeedMps() const { return N > 0 ? SumSpeedMps / double(N) : 0.0; }
};

// Two segments, each with a per-window view and a cumulative view.
//
// CUMULATIVE IS WHAT THE GOAL IS JUDGED ON, and it is printed on every window
// precisely so that `grep | tail -1` -- which has produced four retractions on
// this project by landing on the post-flight linger -- cannot pick up a window
// with three frames in it and call it the answer. A cumulative row is monotone
// and carries its own n.
// THREE STATES, NOT TWO. Fill / settled-parked / settled-moving. See the
// header for the S1 numbers that forced the third one.
FDist FillWindow, FillTotal;
FDist ParkWindow, ParkTotal;      // SETTLED, anchor speed below the threshold
FDist MoveWindow, MoveTotal;      // SETTLED, anchor speed at or above it -- THE GATE
bool bSettled = false;
double SettleSeconds = -1.0;
int64 SelfCheckFailures = 0;      // hitches > n, ever. See EmitDist.

// bGate: only the SETTLED-MOVING rows carry a GOAL3 verdict. Everything else
// prints "gate=n/a" with the reason, so no reader can lift a PASS off a parked
// row -- which is exactly what nearly happened on S1.
void EmitDist(const TCHAR* Seg, const TCHAR* Scope, const FDist& D, bool bGate)
{
	if (D.N == 0)
	{
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame dist seg=%s scope=%s n=0 -- NO FRAMES IN THIS SEGMENT. ")
		       TEXT("If this is SETTLED-MOVING, the leg never flew after settle (or hook 1 is not ")
		       TEXT("receiving anchor speed) and NO >100 FPS claim may be made from it."), Seg, Scope);
		return;
	}

	// SELF-CHECK, AT THE SITE, BECAUSE A FIELD-ORDER SLIP ALREADY HAPPENED.
	// A reader mis-parsed these rows three times and the tell was hitches=46
	// against n=24 -- a hitch count larger than the sample count, which is
	// arithmetically impossible and so is proof of a parse error rather than a
	// bad frame. It can no longer be silent: the impossible pair is named here,
	// as an Error, with both numbers, so the log itself says "you have mis-read
	// this" instead of leaving it to be noticed.
	if (D.Hitches > D.N)
	{
		++SelfCheckFailures;
		UE_LOG(LogVoxelPerf, Error,
		       TEXT("Voxel frame dist SELF-CHECK FAILED seg=%s scope=%s hitches=%lld n=%lld -- ")
		       TEXT("hitches CANNOT exceed n. This is an instrument or parse fault, not a slow frame; ")
		       TEXT("do not read any percentile on this line."),
		       Seg, Scope, (long long)D.Hitches, (long long)D.N);
	}

	const double P50 = D.Quantile(0.50);
	const double P95 = D.Quantile(0.95);
	const double P99 = D.Quantile(0.99);

	// THE GATE HAS TWO PARTS AND THEY ARE JUDGED AND PRINTED SEPARATELY, so a
	// half-pass can never be reported as a pass:
	//
	//   part 1  p95 < 10.00 ms                    -- "above 100 FPS"
	//   part 2  stutters ~ 0 (>= 20.00 ms frames) -- "steady"
	//
	// "~0" IS THE OWNER'S WORD AND kSteadyPct IS MY OPERATIONALISATION OF IT,
	// flagged as such rather than smuggled in: 0.10% is one dropped frame per
	// thousand, about one every ten seconds at 100 fps. It is a switch
	// (-VoxelFramePhaseSteadyPct=) precisely because it is a judgement call and
	// the owner may want it at a hard zero. The RAW stutter count is printed
	// beside it either way, so the verdict can be re-derived under any bar.
	const double StutterPct = 100.0 * double(D.Stutters) / double(D.N);
	const bool bPassP95 = P95 < kFpsGateMs;
	const bool bPassSteady = StutterPct <= SteadyPct();
	// SPEED IS PART OF THE GATE TOO (>= 20 m/s). A MOVING segment that averaged
	// 4 m/s did not test it, and would otherwise print a PASS indistinguishable
	// from one that did. Reported always; enforced only where the gate applies.
	const bool bSpeedQualifies = !bGate || D.MeanSpeedMps() >= GateSpeedMps();

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel frame dist seg=%s scope=%s n=%lld hitches=%lld stutters=%lld stutterPct=%.2f ")
	       TEXT("meanMs=%.2f p50Ms=%.2f p50Fps=%.0f p95Ms=%.2f p95Fps=%.0f p99Ms=%.2f p99Fps=%.0f ")
	       TEXT("maxMs=%.2f meanSpeedMps=%.1f maxSpeedMps=%.1f binMs=%.2f/%.2f ")
	       TEXT("hitchBarMs=%.2f stutterBarMs=%.2f gateP95=%s gateSteady=%s gateSpeed=%s gate=%s"),
	       Seg, Scope, (long long)D.N, (long long)D.Hitches, (long long)D.Stutters, StutterPct,
	       D.Mean(),
	       P50, P50 > 0 ? 1000.0 / P50 : 0.0,
	       P95, P95 > 0 ? 1000.0 / P95 : 0.0,
	       P99, P99 > 0 ? 1000.0 / P99 : 0.0,
	       D.MaxMs, D.MeanSpeedMps(), D.MaxSpeedMps, kFineMs, kCoarseMs,
	       kHitchMs, kStutterMs,
	       bGate ? (bPassP95 ? TEXT("PASS") : TEXT("FAIL")) : TEXT("n/a"),
	       bGate ? (bPassSteady ? TEXT("PASS") : TEXT("FAIL")) : TEXT("n/a"),
	       bGate ? (bSpeedQualifies ? TEXT("PASS") : TEXT("BELOW-20MPS-LEG-DOES-NOT-TEST-GATE")) : TEXT("n/a"),
	       !bGate ? TEXT("n/a-not-settled-moving")
	              : (bPassP95 && bPassSteady && bSpeedQualifies) ? TEXT("GOAL3-PASS")
	                                                             : TEXT("GOAL3-FAIL"));
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
	// UNCLAMPED SINCE 2026-08-23, AND THE CLAMP WAS HIDING THE ANSWER.
	//
	// It used to be Max(0, ...), on the argument that a negative "other" is an
	// artifact of the one-frame lag rather than work that un-happened. On the
	// M20/M30 flying windows that clamp fired on 12 of 16 rows and printed
	// gameOther=0.00 -- which reads as "there is no other game-thread work" and
	// is indistinguishable from "GGameThreadTime came in BELOW the tick and the
	// instrument swallowed the difference". Those are different findings and the
	// clamp made them the same line.
	//
	// A BUCKET THAT CANNOT GO NEGATIVE IS NOT A MEASUREMENT, for the same reason
	// a residual that cannot stay large is not a residual: it silently absorbs
	// its own error. Unclamped, gameOther can print negative, the residual stays
	// exactly frame - tick - other - wait, and the pair says which case it is.
	double MOther() const { return M(GameThread) - M(Tick); }
	double MResidual() const { return M(Frame) - M(Tick) - MOther() - M(GameWait); }
	// The two PIPELINE STAGES, which is what actually decides thread ownership.
	// Game-thread BUSY is GGameThreadTime and nothing else: the engine computes
	// it as (frame period - waits), so it already excludes the blocking wait and
	// is directly comparable to the render thread's own busy time.
	double MGameBusy() const { return M(GameThread); }
	double MRenderBusy() const { return M(Render); }
};

// HEAVY/LIGHT answers the lifted-cap question (does applying 1,024 chunks in a
// frame create render-thread work?). MOVING/PARKED answers the one that now
// decides Goal 3: WHAT MAKES A FLYING FRAME 43-51 ms WHEN A PARKED FRAME IS 9?
// Both are settled-only for the move pair -- a fill frame is neither, and
// mixing it in would put the load storm back into the number the segmentation
// exists to keep out of it.
FBucket All, Heavy, Light, SMoving, SParked;
// CUMULATIVE TWINS, AND THEY EXIST BECAUSE THE PER-WINDOW DELTA WAS BLIND.
//
// On M20 and M30, 51 of 53 windows printed DELTA tag=MOVE VERDICT=NOT-COMPUTED.
// The reason is structural, not a bug: a window in which the camera flew the
// WHOLE window contains no parked frames, so the pair has an empty side and the
// delta refuses -- correctly, by its own "a window without both populations
// says nothing" rule. The only windows that computed were the two TRANSITION
// windows at the start and end of the flight, with n=196/21 and n=46/482:
// tiny, unbalanced, and describing the moment of takeoff rather than the
// flight.
//
// So the delta that answers the Goal 3 question has to be taken across the LEG,
// not within a window: every settled-moving frame against every settled-parked
// frame. These accumulate for the whole run and are never reset.
FBucket SMovingTotal, SParkedTotal;
double LastFrameSeconds = 0.0;
double LastLogSeconds = 0.0;
int32 PeakAppliesThisWindow = 0;
int64 ZeroGameThreadFrames = 0; // hard-zero guard; see the header
int64 NegativeResidualFrames = 0;

void Emit(const TCHAR* Name, const FBucket& B)
{
	if (B.N == 0)
	{
		UE_LOG(LogVoxelPerf, Log, TEXT("Voxel frame phase seg=%s n=0 -- THIS POPULATION IS EMPTY. ")
		                          TEXT("Nothing below describes it and no verdict may be drawn from it."), Name);
		return;
	}
	const double F = B.M(B.Frame);
	auto Pct = [F](double V) { return F > 0 ? 100.0 * V / F : 0.0; };

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel frame phase seg=%s n=%d applPerFrame=%.1f frameMs=%.2f tickMs=%.2f tickPct=%.0f ")
	       TEXT("gameOtherMs=%.2f gameOtherPct=%.0f gameWaitMs=%.2f gameWaitPct=%.0f ")
	       TEXT("residualMs=%.2f residualPct=%.0f renderMs=%.2f renderPct=%.0f ")
	       TEXT("renderWaitMs=%.2f rhiMs=%.2f"),
	       Name, B.N, double(B.Applies) / double(B.N),
	       F, B.M(B.Tick), Pct(B.M(B.Tick)),
	       B.MOther(), Pct(B.MOther()), B.M(B.GameWait), Pct(B.M(B.GameWait)),
	       B.MResidual(), Pct(B.MResidual()),
	       B.MRenderBusy(), Pct(B.MRenderBusy()), B.M(B.RenderWait), B.M(B.RHI));

	// THE PIPELINE LINE -- WHICH THREAD IS THE LONGER STAGE.
	//
	// This is the question the four buckets above cannot answer on their own,
	// because they describe ONE thread's frame and the ceiling may belong to the
	// other. Game-thread busy and render-thread busy are both wall time on a
	// single thread, so they are directly comparable; the frame cannot be
	// shorter than the longer of them, whatever the game thread's own split
	// looks like.
	//
	// GAMEBOUND  game busy is the longer stage -- game-thread microseconds pay.
	// RENDERBOUND render busy is the longer stage -- game-thread work can be
	//            deleted entirely and the frame will not move below it. Every
	//            per-chunk game-thread optimisation is then aimed at the wrong
	//            thread, and that is a claim worth printing rather than
	//            leaving to be inferred from two numbers on different lines.
	// BALANCED   within 15%: neither dominates and both must come down.
	//
	// headroomMs is what the frame WOULD be if the shorter stage vanished: the
	// floor any single-thread fix is working against.
	const double GameBusy = B.MGameBusy();
	const double RenderBusy = B.MRenderBusy();
	const double Longer = FMath::Max(GameBusy, RenderBusy);
	const double Ratio = Longer > 0 ? FMath::Abs(GameBusy - RenderBusy) / Longer : 0.0;
	const TCHAR* Bound = (GameBusy <= 0.0 || RenderBusy <= 0.0) ? TEXT("UNKNOWN-A-STAGE-READS-ZERO")
	                   : (Ratio < 0.15)          ? TEXT("BALANCED")
	                   : (GameBusy > RenderBusy) ? TEXT("GAMEBOUND")
	                                             : TEXT("RENDERBOUND");
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel frame phase PIPELINE seg=%s gameBusyMs=%.2f renderBusyMs=%.2f frameMs=%.2f ")
	       TEXT("longerStageMs=%.2f floorFps=%.0f gapPct=%.0f bound=%s"),
	       Name, GameBusy, RenderBusy, F, Longer,
	       Longer > 0 ? 1000.0 / Longer : 0.0, 100.0 * Ratio, Bound);
}

// One delta, one verdict, for any pair of buckets. Factored so the two
// questions -- moving vs parked, and heavy vs light apply -- are answered by
// exactly the same arithmetic and the same disproof thresholds, rather than by
// two hand-written blocks that could drift apart.
void EmitDelta(const TCHAR* Tag, const TCHAR* HiName, const TCHAR* LoName,
               const FBucket& Hi, const FBucket& Lo, const TCHAR* Question)
{
	if (Hi.N == 0 || Lo.N == 0)
	{
		// The "read the peak window" guard. A window missing either population
		// describes only the state it was in and is not evidence about the other.
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame phase DELTA tag=%s VERDICT=NOT-COMPUTED nHi=%d nLo=%d (%s..%s) -- ")
		       TEXT("a window without BOTH populations says nothing about the comparison."),
		       Tag, Hi.N, Lo.N, HiName, LoName);
		return;
	}

	const double DFrame = Hi.M(Hi.Frame) - Lo.M(Lo.Frame);
	const double DTick = Hi.M(Hi.Tick) - Lo.M(Lo.Tick);
	const double DOther = Hi.MOther() - Lo.MOther();
	const double DWait = Hi.M(Hi.GameWait) - Lo.M(Lo.GameWait);
	const double DRes = Hi.MResidual() - Lo.MResidual();
	const double DRender = Hi.M(Hi.Render) - Lo.M(Lo.Render);
	const double DRenderWait = Hi.M(Hi.RenderWait) - Lo.M(Lo.RenderWait);
	const double DRHI = Hi.M(Hi.RHI) - Lo.M(Lo.RHI);
	auto Share = [DFrame](double V) { return FMath::Abs(DFrame) > 1e-6 ? 100.0 * V / DFrame : 0.0; };

	// Every share beside its own absolute ms. waitShare=99% was true and
	// meaningless tonight; a share with no millisecond next to it is not a
	// measurement on this project any more.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel frame phase DELTA tag=%s (%s minus %s, n=%d/%d) dFrameMs=%+.2f ")
	       TEXT("dTickMs=%+.2f dTickPct=%.0f dOtherMs=%+.2f dOtherPct=%.0f dGameWaitMs=%+.2f ")
	       TEXT("dGameWaitPct=%.0f dResidualMs=%+.2f dResidualPct=%.0f dRenderMs=%+.2f ")
	       TEXT("dRenderPct=%.0f dRenderWaitMs=%+.2f dRhiMs=%+.2f | q=%s"),
	       Tag, HiName, LoName, Hi.N, Lo.N, DFrame,
	       DTick, Share(DTick), DOther, Share(DOther), DWait, Share(DWait),
	       DRes, Share(DRes), DRender, Share(DRender), DRenderWait, DRHI, Question);

	const bool bH1Disproved = FMath::Abs(Share(DRender)) < 20.0 && FMath::Abs(Share(DWait)) < 20.0;
	const bool bH2Disproved = FMath::Abs(Share(DTick) + Share(DOther)) < 50.0;
	const bool bH3 = Share(DRes) > 50.0;
	const TCHAR* Verdict =
		FMath::Abs(DFrame) < 1.0
			? TEXT("NO-FRAME-DIFFERENCE -- this split does not move frame time; the ceiling is not visible from here")
		: bH3
			? TEXT("H3-UNATTRIBUTED -- the residual carries it; NOT the render thread, NOT the streaming tick")
		: (bH1Disproved && bH2Disproved)
			? TEXT("H1-AND-H2-BOTH-DISPROVED -- neither thread's named work explains it; read the residual")
		: bH1Disproved
			? TEXT("H1-DISPROVED -- NOT the render thread")
		: bH2Disproved
			? TEXT("H2-DISPROVED -- NOT game-thread work")
			: TEXT("NEITHER-DISPROVED -- both threads moved; this instrument cannot separate them");
	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel frame phase DELTA tag=%s VERDICT=%s"), Tag, Verdict);
}

void Flush(double Now)
{
	const double WindowSec = Now - LastLogSeconds;

	if (Mode() & kModeDistribution)
	{
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame dist window=%.1fs settleT=%s moveThreshUU=%.0f gateMs=%.2f ")
		       TEXT("gateMps=%.1f steadyPct=%.2f selfCheckFailures=%lld ")
		       TEXT("-- GOAL3 IS JUDGED ON seg=SETTLED-MOVING ONLY"),
		       WindowSec,
		       SettleSeconds >= 0.0 ? *FString::Printf(TEXT("%.1f"), SettleSeconds) : TEXT("NOT-SETTLED"),
		       MoveThresholdUU(), kFpsGateMs, GateSpeedMps(), SteadyPct(),
		       (long long)SelfCheckFailures);

		EmitDist(TEXT("FILL"),           TEXT("window"), FillWindow, false);
		EmitDist(TEXT("SETTLED-PARKED"), TEXT("window"), ParkWindow, false);
		EmitDist(TEXT("SETTLED-MOVING"), TEXT("window"), MoveWindow, true);
		EmitDist(TEXT("FILL"),           TEXT("total"),  FillTotal,  false);
		EmitDist(TEXT("SETTLED-PARKED"), TEXT("total"),  ParkTotal,  false);
		EmitDist(TEXT("SETTLED-MOVING"), TEXT("total"),  MoveTotal,  true);

		FillWindow = ParkWindow = MoveWindow = FDist{};
	}

	if (Mode() & kModeReconcile)
	{
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame phase window=%.1fs heavyAppl>=%d lightAppl<%d peakApplPerFrame=%d ")
		       TEXT("zeroGameThreadFrames=%lld negResidualFrames=%lld"),
		       WindowSec, HeavyThreshold(), kLightThreshold, PeakAppliesThisWindow,
		       (long long)ZeroGameThreadFrames, (long long)NegativeResidualFrames);
		Emit(TEXT("ALL"),            All);
		Emit(TEXT("LIGHT-APPLY"),    Light);
		Emit(TEXT("HEAVY-APPLY"),    Heavy);
		Emit(TEXT("SETTLED-PARKED"), SParked);
		Emit(TEXT("SETTLED-MOVING"), SMoving);
		Emit(TEXT("SETTLED-PARKED-LEG"), SParkedTotal);
		Emit(TEXT("SETTLED-MOVING-LEG"), SMovingTotal);

		// THE MOVE DELTA IS THE GOAL 3 QUESTION and is emitted first for that
		// reason: what makes a FLYING frame 43-51 ms when a PARKED frame is 9?
		// Same build, same settled state, same everything but the anchor moving.
		// LEG SCOPE FIRST -- it is the one that computes on a fully-flying
		// window, and therefore the one that answers the question. The
		// window-scope delta below it is kept because a transition window is a
		// genuine within-5-seconds comparison, immune to any drift across the
		// leg; it just cannot fire while the camera is simply flying.
		EmitDelta(TEXT("MOVE-LEG"), TEXT("settled-moving-leg"), TEXT("settled-parked-leg"),
		          SMovingTotal, SParkedTotal,
		          TEXT("what-makes-a-FLYING-frame-slow--THE-GOAL3-QUESTION"));
		EmitDelta(TEXT("MOVE"), TEXT("settled-moving"), TEXT("settled-parked"), SMoving, SParked,
		          TEXT("same-question-within-one-window--fires-only-on-transition-windows"));
		EmitDelta(TEXT("LOAD"), TEXT("heavy-apply"), TEXT("light-apply"), Heavy, Light,
		          TEXT("what-applying-many-chunks-in-one-frame-costs--the-lifted-cap-question"));
	}

	All = Heavy = Light = SMoving = SParked = FBucket{};
	PeakAppliesThisWindow = 0;
	ZeroGameThreadFrames = 0;
	NegativeResidualFrames = 0;
	LastLogSeconds = Now;
}
} // namespace

void NoteFrameImpl(double VoxelTickMs, int32 AppliesThisFrame, double AnchorSpeedUUPerSec)
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
	// THE THIRD STATE. Moving is decided by the SAME anchor-speed EMA and the
	// SAME 100 UU/s constant admission's velocity-bias path already uses -- one
	// opinion about what "moving" means, not two.
	const bool bMoving = AnchorSpeedUUPerSec >= MoveThresholdUU();
	if (Mode() & kModeDistribution)
	{
		if (!bSettled)
		{
			FillWindow.Add(FrameMs, AnchorSpeedUUPerSec);
			FillTotal.Add(FrameMs, AnchorSpeedUUPerSec);
		}
		else if (bMoving)
		{
			MoveWindow.Add(FrameMs, AnchorSpeedUUPerSec);
			MoveTotal.Add(FrameMs, AnchorSpeedUUPerSec);
		}
		else
		{
			ParkWindow.Add(FrameMs, AnchorSpeedUUPerSec);
			ParkTotal.Add(FrameMs, AnchorSpeedUUPerSec);
		}
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

	// SETTLED ONLY for the move pair. A fill frame is neither moving-after-settle
	// nor parked-after-settle, and folding it in would put the load storm back
	// into the very number the segmentation exists to keep it out of.
	if (bSettled)
	{
		(bMoving ? SMoving : SParked)
			.Add(FrameMs, VoxelTickMs, GameMs, GameWaitMs, RenderMs, RenderWaitMs, RHIMs, AppliesThisFrame);
		(bMoving ? SMovingTotal : SParkedTotal)
			.Add(FrameMs, VoxelTickMs, GameMs, GameWaitMs, RenderMs, RenderWaitMs, RHIMs, AppliesThisFrame);
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
	       TEXT("Voxel frame dist BOUNDARY settleT=%.1f fillN=%lld fillP95Ms=%.2f fillMaxMs=%.2f ")
	       TEXT("fillHitches=%lld -- every frame from here is SETTLED, and splits again into ")
	       TEXT("SETTLED-MOVING (>=%.0f UU/s, THE GATE) and SETTLED-PARKED (not the gate). ")
	       TEXT("Fill is exempt; nothing after this line is."),
	       InSettleSeconds, (long long)FillTotal.N, FillTotal.Quantile(0.95),
	       FillTotal.MaxMs, (long long)FillTotal.Hitches, MoveThresholdUU());
}

} // namespace VoxelFramePhase
