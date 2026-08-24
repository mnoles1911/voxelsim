// VoxelTickBudget.h -- one clock for the streaming tick, so a burst becomes
// five ordinary frames instead of one 100 ms frame.
//
// ===========================================================================
// WHY REDISTRIBUTION AND NOT A CONSTANT-FACTOR WIN
// ===========================================================================
//
// The M20/M30 legs showed the flying tail is BIMODAL, not a general slowdown
// (docs/flight-tail-named-2026-08-24.md section 4). Going 20 -> 30 m/s:
//
//     stutters (>=20 ms)   116-138 (51-66%)  ->   91-108 (48-51%)   DOWN
//     hitches  (>=33.3 ms)      38-49        ->       50-63         UP
//     p50                    20.9-24.5 ms    ->    17.8-20.9 ms     DOWN
//     p99 / max              57-71 / 73-90   ->   77-106 / 99-120    UP
//
// Mass left the 20-33 ms band in BOTH directions while the mean barely moved.
// Terrain difficulty moves mean and median together and could not do that. The
// tail is A MINORITY OF VERY EXPENSIVE FRAMES.
//
// The steadiness gate is stutters 31.4% -> 0.10%: A 300x REDUCTION IN TAIL
// FRAMES. No constant-factor win on any term reaches that. If one frame in
// twenty does 100 ms of work, halving the work leaves one frame in twenty at
// 50 ms and it still fails. Spreading the SAME work over five frames at 20 ms
// passes. Only redistribution reaches a 300x.
//
// This is also the one lever that may move both halves of the gate at once: the
// render lane's prior is that the per-frame render commands scale with
// streaming and execute outside the scene renderer, so spreading streaming work
// spreads render-thread work with it. The render thread is RENDERBOUND at every
// speed and a game-thread budgeter cannot reach 100 fps alone -- but steadiness
// is a SEPARATE pass/fail and is where a bimodal tail lives.
//
// ===========================================================================
// DESIGN QUESTION 1: WHAT IS THE UNIT OF DEFERRAL?
// ===========================================================================
//
// A WHOLE STAGE, AT ITS OWN EXISTING LOOP BOUNDARY. Not a cell range, not a
// level, and nothing new.
//
// The reason is the rule this codebase has paid the most for: its worst
// failures are silent wrong terrain, not crashes. A novel mid-scan resumption
// point would be new torn state by construction, and the recorded example is
// exact -- DrainResults once popped a result off the MPSC queue and broke out
// of the body before using it, stranding the chunk with bJobInFlight pinned and
// no counter moving.
//
// So this defers only where the code ALREADY stops early and is ALREADY known
// to resume cleanly:
//
//   DrainResults        already exits on MaxAppliesPerFrame / ApplyBudgetMs.
//                       Work lives in ResultsQueue and persists.
//   DrainGameThreadMesh already exits on MaxRemeshesPerFrame.
//                       Work lives in PendingGameThreadKeys and persists.
//   DrainUnloads        already exits on MaxUnloadsPerFrame.
//   DispatchJobs        already exits on its in-flight caps.
//   RecomputeDesiredSet CANNOT be interrupted mid-scan -- so it is never
//                       interrupted. It is deferred by DECLINING TO START,
//                       which -VoxelRecomputeDutyPct already does today and
//                       has already been proven safe: the trigger
//                       (bRecomputeWanted) is recomputed from the anchor every
//                       tick, so a declined recompute simply happens next tick.
//
// Every one of those is a point where the containing collection IS the state.
// Nothing is dropped, because nothing is removed from a collection without
// being processed. THE DEFERRED WORK IS CARRIED BY THE QUEUE THAT ALREADY OWNS
// IT, which is why no new carry structure appears in this file -- a new carry
// structure would be a second place for work to be lost.
//
// ===========================================================================
// DESIGN QUESTION 2: WHAT HAPPENS UNDER SUSTAINED OVERLOAD?
// ===========================================================================
//
// The recorded failure is VoxelWorldSubsystem.cpp:3571-3579 -- R3/R4 at 0
// loaded chunks for 90 s, from an ordering change. A budget that always spent
// itself on the earliest stage would reproduce it exactly. Three guards, and
// the third makes starvation impossible rather than unlikely:
//
//   1. URGENT IS NEVER DEFERRED. The recompute gate already distinguishes
//      bRecomputeUrgent (first recompute, underground transition) and bypasses
//      the existing rate bound for it. This budget honours the same exemption
//      through the same expression, so a cold start and a world transition
//      cannot be deferred at all.
//
//   2. A RESERVE FOR LATE STAGES. Stage order is dispatch, apply, remesh,
//      dispatch, unload. Without a reserve, apply could consume the whole
//      budget every tick and unloads would never run -- residency grows without
//      bound and the symptom is memory, not a stall. ReserveMs is held back
//      from every stage except the last.
//
//   3. ESCALATION AFTER N CONSECUTIVE DEFERRALS. After -VoxelTickBudgetMaxDefer
//      consecutive declines (default 4), the stage runs REGARDLESS of budget.
//      This bounds worst-case latency to N ticks BY CONSTRUCTION rather than by
//      argument: at ~50 Hz, 4 ticks is 80 ms, and no ring can be starved beyond
//      that however long the overload lasts. escalations= and
//      maxConsecutiveDefer= are counted so the bound can be seen binding.
//
// A BUDGET THAT CANNOT ESCALATE IS A THROTTLE. The whole difference between
// redistribution and throttling is whether deferred work is guaranteed to run.
//
// AND THE CHECK IS PER-RING RESIDENCY, NEVER AGGREGATE THROUGHPUT. Aggregate
// chunks/s stayed healthy through the recorded R3/R4 stall. Read the per-ring
// loaded= on the streaming line; any ring at 0 for more than 10 s is a revert
// whatever the tail did.
//
// ===========================================================================
// DESIGN QUESTION 3: HOW DOES IT INTERACT WITH THE APPLY BUDGETS?
// ===========================================================================
//
// voxel.Stream.MaxAppliesPerFrame and voxel.Stream.ApplyBudgetMs are already
// per-frame budgets, and two budgets that do not know about each other is how a
// third invisible ceiling appears. This one subordinates them rather than
// competing:
//
//   ApplyBudgetMs   BECOMES TICK-AWARE, through the interception that already
//                   exists: VoxelApplyFast::ApplyBudgetSeconds() is already the
//                   single place DrainResults gets its wall budget, so it now
//                   returns min(its own value, what the tick has left minus the
//                   reserve). NO NEW HOOK. One clock, one owner.
//
//   MaxAppliesPerFrame  IS LEFT ALONE, deliberately. It is a COUNT, and the
//                   apply-ceiling census established it is a safety ceiling
//                   rather than a throttle. Making a count cap tick-aware would
//                   put two currencies on one clock and make the exit
//                   attribution unreadable -- and that census
//                   (countCap/wallClock/queueEmpty/drainCap) is exactly how
//                   anyone tells this budget apart from the ones underneath it.
//
// THE EXISTING EXIT CENSUS IS THIS FEATURE'S PROOF OF TRAFFIC. Under a binding
// tick budget, wallClock must RISE and countCap must FALL, because the loop is
// now stopping on a clock it did not previously have. If neither moves, the
// budget is not reaching DrainResults at all.
//
// ===========================================================================
// REGISTERED DISPROOF -- WRITTEN BEFORE THE LEG
// ===========================================================================
//
//   THE ONE THAT CONDEMNS IT: stutterPct falls while COLD START rises by more
//   than the deferred work explains -> this is THROTTLING, not redistribution,
//   and it must be REVERTED rather than tuned. Operationalised so it is a
//   comparison and not a judgement: deferredMs is summed per window and
//   reported. If the cold-start regression in milliseconds EXCEEDS the total
//   deferredMs accumulated over the fill, the budget destroyed throughput it
//   never carried. Work that is genuinely carried costs cold start AT MOST the
//   amount deferred, and less in practice because it overlaps.
//
//   escalations ~= deferrals -> the budget is not deferring, it is adding a
//   branch and then doing the work anyway. Traffic without effect.
//
//   deferrals = 0 with the switch on -> the budget never bound. Nothing in this
//   file ran, and any tail change came from somewhere else. Raising
//   -VoxelTickBudgetMs is the WRONG response until it is known the hook fired
//   at all: read hookedTicks= first.
//
//   hookedTicks = 0 -> hook A was not applied. The module is linked and latched
//   and completely inert.
//
//   any per-ring loaded= at 0 for >10 s -> the recorded R3/R4 stall, reproduced.
//   REVERT regardless of what the tail did. Aggregate throughput cannot see it.
//
//   p50 improves while p99/max/stutterPct do not -> the budget moved the median
//   and left the tail, which is the exact opposite of the point. THE READING
//   THAT DECIDES THIS FEATURE IS p99, max AND stutterPct ON SETTLED-MOVING --
//   NOT p50 -- with cold start reported beside it, because an arm that fixes
//   the tail and costs cold start is a trade the owner must see rather than one
//   this file makes quietly.
//
#pragma once

#include "CoreMinimal.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace VoxelTickBudget
{
// -VoxelTickBudgetMs=F. 0 = OFF = default = byte-identical: every entry point
// below returns "unbounded" and touches no state.
inline double BudgetMs()
{
	static const double Latched = []
	{
		float Value = 0.f;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelTickBudgetMs="), Value);
		return double(FMath::Max(0.f, Value));
	}();
	return Latched;
}

inline bool Enabled() { return BudgetMs() > 0.0; }

// Consecutive deferrals after which a stage runs regardless. Bounds worst-case
// latency to N ticks BY CONSTRUCTION. See design question 2, guard 3.
inline int32 MaxConsecutiveDefer()
{
	static const int32 Latched = []
	{
		int32 Value = 4;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelTickBudgetMaxDefer="), Value);
		return FMath::Max(1, Value);
	}();
	return Latched;
}

// Held back from every stage but the last, so an early stage cannot consume the
// whole tick and starve unloads. See design question 2, guard 2.
inline double ReserveMs()
{
	static const double Latched = []
	{
		float Value = 2.0f;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelTickBudgetReserveMs="), Value);
		return double(FMath::Max(0.f, Value));
	}();
	return Latched;
}

// HOOK A. Start of TickStreaming, given the tick's own start timestamp so this
// shares the clock the tick is already measured against rather than starting a
// second one a few microseconds later.
void BeginTickImpl(double TickStartSeconds);
FORCEINLINE void BeginTick(double TickStartSeconds)
{
	if (Enabled()) { BeginTickImpl(TickStartSeconds); }
}

// HOOK B. The recompute gate. bUrgent comes straight from bRecomputeUrgent and
// is honoured exactly as the existing rate bound honours it: an urgent
// recompute is NEVER deferred.
//
// Returns true = run it. With the switch off, always true, no state touched.
bool MayStartRecomputeImpl(bool bUrgent);
FORCEINLINE bool MayStartRecompute(bool bUrgent)
{
	return !Enabled() || MayStartRecomputeImpl(bUrgent);
}

// Seconds left in this tick, minus the reserve. Negative means over budget.
// Consumed by VoxelApplyFast::ApplyBudgetSeconds -- NO HOOK. Returns a very
// large number when the switch is off, so callers need no branch of their own.
double RemainingSecondsForStage();

// Clamp a stage's requested wall budget to what the tick has left, counting the
// clamp where it happens. Consumed by VoxelApplyFast::ApplyBudgetSeconds --
// NO HOOK, because that function is already the single place DrainResults gets
// its wall budget. Returns RequestedSeconds unchanged when the switch is off.
double ClampStageSeconds(double RequestedSeconds);

// Self-clocked 5 s report; also callable from MaybeLogCounters for exact window
// alignment (optional hook C).
void FlushStats(bool bForce);

} // namespace VoxelTickBudget
