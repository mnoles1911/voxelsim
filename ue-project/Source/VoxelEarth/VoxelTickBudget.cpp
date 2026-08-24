// VoxelTickBudget.cpp -- see VoxelTickBudget.h for why redistribution rather
// than a constant-factor win, the three design questions answered in full, and
// the disproof registered before the leg.

#include "VoxelTickBudget.h"

#include "VoxelDebug.h" // LogVoxelPerf

namespace VoxelTickBudget
{
namespace
{

// GAME THREAD ONLY, like every other piece of streaming state. TickStreaming is
// the sole writer and DrainResults the sole other reader, both on the game
// thread; nothing here is synchronised and nothing here may be called from a
// worker.
double TickStartSeconds = 0.0;
bool bTickOpen = false;

// Consecutive deferrals of the recompute stage. Reset to 0 whenever it runs,
// for any reason -- including an escalation -- so the escalation bound measures
// consecutive DECLINES rather than consecutive wants.
int32 RecomputeConsecutiveDefers = 0;

// --- counters, one 5 s window -----------------------------------------------
int64 HookedTicks = 0;          // BeginTick calls. 0 = hook A not applied.
int64 RecomputeWanted = 0;      // MayStartRecompute calls (non-urgent + urgent)
int64 RecomputeUrgentPass = 0;  // ...allowed through because urgent
int64 RecomputeDeferred = 0;    // ...declined for budget
int64 RecomputeEscalated = 0;   // ...allowed through because the defer bound hit
int64 RecomputeFit = 0;         // ...allowed through because it fitted
int32 MaxConsecutiveSeen = 0;
// Milliseconds of tick time the budget DECLINED to spend, estimated as the
// budget overrun at the moment of the decline. This is the number the
// throttle-vs-redistribute disproof is settled against: work genuinely carried
// costs cold start at most this much.
double DeferredMs = 0.0;
// Stage clamps: how often, and by how much, a stage asked for more wall than
// the tick had left.
int64 StageClamps = 0;
double StageClampedMs = 0.0;
int64 StageQueries = 0;

double LastLogSeconds = 0.0;

void MaybeLog(double Now)
{
	if (LastLogSeconds <= 0.0)
	{
		LastLogSeconds = Now;
		return;
	}
	if (Now - LastLogSeconds >= 5.0)
	{
		FlushStats(/*bForce*/ true);
	}
}

} // namespace

void FlushStats(bool bForce)
{
	if (!Enabled())
	{
		return; // control arm prints nothing at all
	}
	const double Now = FPlatformTime::Seconds();
	if (LastLogSeconds <= 0.0)
	{
		LastLogSeconds = Now;
		return;
	}
	if (!bForce && Now - LastLogSeconds < 5.0)
	{
		return;
	}
	const double WindowSec = FMath::Max(1e-6, Now - LastLogSeconds);

	// STRICT key=value, no spaces inside values, and deferrals beside
	// escalations because those two are the pair a reader must compare: equal
	// means the budget is a branch that defers nothing.
	//
	// hookedTicks= is FIRST because it is the one reading that separates "the
	// budget did not bind" from "the budget was never called". Those demand
	// opposite next moves and have been confused on this project repeatedly.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel tick budget window=%.1fs budgetMs=%.2f reserveMs=%.2f maxDefer=%d ")
	       TEXT("hookedTicks=%lld recomputeWanted=%lld deferrals=%lld escalations=%lld ")
	       TEXT("urgentPass=%lld fitPass=%lld maxConsecutiveDefer=%d deferredMs=%.1f ")
	       TEXT("stageQueries=%lld stageClamps=%lld stageClampedMs=%.1f"),
	       WindowSec, BudgetMs(), ReserveMs(), MaxConsecutiveDefer(),
	       (long long)HookedTicks, (long long)RecomputeWanted, (long long)RecomputeDeferred,
	       (long long)RecomputeEscalated, (long long)RecomputeUrgentPass,
	       (long long)RecomputeFit, MaxConsecutiveSeen, DeferredMs,
	       (long long)StageQueries, (long long)StageClamps, StageClampedMs);

	// The instrument names its own dead readings inline, because the eighth
	// window-selection trap tonight was caught only because one did.
	if (HookedTicks == 0)
	{
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("Voxel tick budget INERT: hookedTicks=0 -- hook A (BeginTick) was never called. ")
		       TEXT("The switch latched and the module is linked, but NOTHING IN IT RAN. Any tail ")
		       TEXT("change this leg shows came from somewhere else."));
	}
	else if (RecomputeDeferred == 0 && StageClamps == 0)
	{
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("Voxel tick budget NEVER BOUND: hookedTicks=%lld but deferrals=0 and ")
		       TEXT("stageClamps=0 -- the budget of %.2f ms was never exceeded, so this arm is ")
		       TEXT("BEHAVIOURALLY IDENTICAL to control. Lower -VoxelTickBudgetMs before reading ")
		       TEXT("anything into the tail."),
		       (long long)HookedTicks, BudgetMs());
	}
	else if (RecomputeDeferred > 0 && RecomputeEscalated >= RecomputeDeferred)
	{
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("Voxel tick budget NOT DEFERRING: escalations=%lld >= deferrals=%lld -- every ")
		       TEXT("decline was immediately overridden by the escalation bound, so the work was ")
		       TEXT("done anyway and this is traffic without effect. Raise ")
		       TEXT("-VoxelTickBudgetMaxDefer or accept that the budget cannot help here."),
		       (long long)RecomputeEscalated, (long long)RecomputeDeferred);
	}

	HookedTicks = RecomputeWanted = RecomputeUrgentPass = 0;
	RecomputeDeferred = RecomputeEscalated = RecomputeFit = 0;
	StageClamps = StageQueries = 0;
	MaxConsecutiveSeen = 0;
	DeferredMs = 0.0;
	StageClampedMs = 0.0;
	LastLogSeconds = Now;
}

void BeginTickImpl(double InTickStartSeconds)
{
	TickStartSeconds = InTickStartSeconds;
	bTickOpen = true;
	++HookedTicks;
	MaybeLog(FPlatformTime::Seconds());
}

double RemainingSecondsForStage()
{
	if (!Enabled() || !bTickOpen)
	{
		// UNBOUNDED, and deliberately a huge finite number rather than infinity:
		// callers take a Min() against it and a NaN or an inf would propagate
		// silently into a wall-clock comparison. With the switch off this is the
		// only statement that runs.
		return 1.0e9;
	}
	++StageQueries;
	const double ElapsedSec = FPlatformTime::Seconds() - TickStartSeconds;
	const double RemainingSec = (BudgetMs() - ReserveMs()) / 1000.0 - ElapsedSec;
	return RemainingSec;
}

bool MayStartRecomputeImpl(bool bUrgent)
{
	++RecomputeWanted;

	// GUARD 1. Urgent is never deferred -- the first recompute of a session and
	// an underground transition. The existing rate bound already exempts exactly
	// this case and for the same reason: deferring it would defer the world
	// existing at all.
	if (bUrgent)
	{
		++RecomputeUrgentPass;
		RecomputeConsecutiveDefers = 0;
		return true;
	}

	// GUARD 3. The escalation bound, checked BEFORE the budget test so that a
	// stage which has waited its N ticks runs even if the tick is already deep
	// over budget. Checking it after would let a sustained overload defer
	// forever, which is the R3/R4 stall with a different name.
	if (RecomputeConsecutiveDefers >= MaxConsecutiveDefer())
	{
		++RecomputeEscalated;
		RecomputeConsecutiveDefers = 0;
		return true;
	}

	if (!bTickOpen)
	{
		// Hook A missing while hook B is present. Fail OPEN -- run the work --
		// because a half-applied pair must never be able to withhold work. The
		// hookedTicks=0 warning in FlushStats is what makes this visible rather
		// than merely safe.
		++RecomputeFit;
		return true;
	}

	const double ElapsedSec = FPlatformTime::Seconds() - TickStartSeconds;
	const double BudgetSec = (BudgetMs() - ReserveMs()) / 1000.0;

	// THE COST IS NOT PREDICTED HERE, and that is deliberate. The impl already
	// keeps RecomputeCostEMASec for exactly this purpose, but reading it would
	// need a second hook and a second opinion about the same quantity. The test
	// is the simpler and more conservative one: if the tick has ALREADY spent
	// its budget, do not start a scan measured at 4.90-6.84 ms on the M20 leg.
	// A scan that starts inside its budget is allowed to overrun it -- it cannot
	// be interrupted, so pretending otherwise would be theatre.
	if (ElapsedSec >= BudgetSec)
	{
		++RecomputeDeferred;
		++RecomputeConsecutiveDefers;
		MaxConsecutiveSeen = FMath::Max(MaxConsecutiveSeen, RecomputeConsecutiveDefers);
		// The overrun at the moment of declining: what this decision moved out
		// of this frame. Summed into the number the throttle-vs-redistribute
		// disproof is settled against.
		DeferredMs += (ElapsedSec - BudgetSec) * 1000.0;
		return false;
	}

	++RecomputeFit;
	RecomputeConsecutiveDefers = 0;
	return true;
}

// Called by VoxelApplyFast::ApplyBudgetSeconds. Separate from
// RemainingSecondsForStage only so the clamp can be counted where it happens.
double ClampStageSeconds(double RequestedSeconds)
{
	if (!Enabled() || !bTickOpen)
	{
		return RequestedSeconds;
	}
	const double Remaining = RemainingSecondsForStage();
	if (Remaining >= RequestedSeconds)
	{
		return RequestedSeconds;
	}
	++StageClamps;
	StageClampedMs += (RequestedSeconds - FMath::Max(0.0, Remaining)) * 1000.0;
	// NEVER BELOW ZERO. DrainResults applies a kMinAppliesPerFrame floor before
	// it consults the wall budget at all, so a zero here does not stall the
	// pipeline -- it makes the loop take its floor and yield, which is exactly
	// the redistribution this file exists to cause.
	return FMath::Max(0.0, Remaining);
}

} // namespace VoxelTickBudget
