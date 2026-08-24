// VoxelFramePhase.h -- where a frame goes when the per-tick caps are lifted,
// and which THREAD owns the ceiling.
//
// ===========================================================================
// THE LEAD THAT SENT ME HERE IS FALSIFIED, AND THE FALSIFICATION IS THE FIRST
// THING IN THIS FILE SO NOBODY RE-DERIVES THE THEORY FROM IT
// ===========================================================================
//
// The hitch line that opened this investigation:
//
//   Hitch frame: frameMs=400.00 | subsystemTickMs=17.36 elsewhereMs=382.64
//                renderMs=1505.40 renderWaitMs=9962.46 rhiMs=0.00 gameWaitMs=0.00
//
// renderWaitMs=9962 on a 400 ms frame reads as catastrophic render-thread
// backpressure. It is not a measurement of anything.
//
// In Saved/ahead-on.log, EXACTLY TWO of 717 hitch lines carry renderWaitMs
// above 200, and they are:
//
//   [2026.08.23-22.25.56:262][  1]  renderWait=9773.87  rhi=0.00  gameWait=0.00
//   [2026.08.23-22.25.56:284][  2]  renderWait=9773.87  rhi=0.00  gameWait=0.00
//
// FRAMES 1 AND 2. Identical value on both -- a global that was not rewritten
// between them. The process's first log line is at 22.25.47, so these are the
// first rendered frames after ~9 s of module load, RHI init and shader setup,
// and GRenderThreadWaitTime is reporting that STARTUP accumulation, once,
// before the per-frame cadence begins. rhiMs=0.00 and gameWaitMs=0.00 on the
// same line are the tell: two of the four timers had never been written at all.
//
// The median over the 715 non-startup hitch frames of the same log is
// renderWait = 18.89 ms. That is the real number. 9962 is frame 1.
//
// This is the third time tonight the first window of a cold leg has produced a
// mechanism that was not there.
//
// ===========================================================================
// WHAT THE EXISTING INSTRUMENT CAN AND CANNOT SAY
// ===========================================================================
//
// voxel.Stream.FrameAttribution already samples every frame and splits FAST
// (<= p50) from SLOW (>= p95) with a DELTA line. It is the right shape, and it
// was never armed on a lifted-cap leg -- q-L2.log (maxApplies=1024,
// budgetMs=24) contains zero attribution lines.
//
// But it cannot answer THIS question even when armed, and the reason is
// structural rather than a bug: render / renderWait / rhi / gameWait are
// PER-THREAD TOTALS THAT OVERLAP EACH OTHER AND THE FRAME. They do not sum to
// frameMs, nothing checks that they do, and so the instrument can say "render
// rose by X" but can never say "and 94 ms is still unaccounted for". The whole
// question I was given is about the unaccounted part.
//
// So this file adds the one thing missing: A RECONCILIATION WITH AN EXPLICIT
// RESIDUAL, printed in absolute milliseconds, that is allowed to come out and
// say NOT THE RENDER THREAD.
//
//     frameMs  =  voxelTickMs + gameOtherMs + gameWaitMs + residualMs
//
//   voxelTickMs  the streaming tick (passed in by the caller)
//   gameOtherMs  GGameThreadTime - voxelTickMs: everything else the game
//                thread did this frame
//   gameWaitMs   GGameThreadWaitTime: the game thread BLOCKED, which in -game
//                is the frame-end sync on the render thread
//   residualMs   frameMs - the three above. THE POINT OF THE FILE.
//
// and, beside it, the render side, which is what decides thread ownership:
//
//   renderMs      render thread busy
//   renderWaitMs  render thread idle, waiting on RHI/GPU
//
// ===========================================================================
// REGISTERED DISPROOF -- WRITTEN BEFORE THE LEG, NOT AFTER
// ===========================================================================
//
// The frames are split by HOW MUCH WORK THE TICK DID, not by how slow they
// were. HEAVY = applied >= -VoxelFramePhaseHeavy (default 512) chunks this
// frame; LIGHT = applied < 64. Both populations come from ONE leg, so
// leg-to-leg variance -- which has already produced a retraction on this
// project when a contended box was read as a slow configuration -- cannot
// enter. It is also why this is not an A/B: the comparison is within a run.
//
//   H1  THE CEILING IS THE RENDER THREAD.
//       DISPROVED IF, heavy vs light:  d(render)   < 20% of d(frame)
//                                 AND  d(gameWait) < 20% of d(frame).
//       If applying 1,024 chunks made no more render-thread work and did not
//       block the game thread any longer, the render thread is not it --
//       whatever renderMs reads in absolute terms.
//
//   H2  THE CEILING IS THE GAME THREAD.
//       DISPROVED IF  d(voxelTick + gameOther) < 50% of d(frame).
//
//   H3  NEITHER; IT IS UNATTRIBUTED.
//       CONFIRMED IF  d(residual) > 50% of d(frame). Then this instrument says
//       so out loud and names nothing. That is a legitimate result and it is
//       better than picking a thread.
//
// H1 and H2 can BOTH be disproved. That is the outcome the residual exists
// for, and it must be reported rather than resolved by preference.
//
// A PERCENTAGE WITHOUT ITS ABSOLUTE IS NOT A MEASUREMENT. Every share printed
// below is printed beside its own millisecond value, because waitShare=99% was
// true and meaningless tonight -- ~1,500 game-thread prefetches spread over ~36
// worker threads, absorbed by parallelism, never wall time. And for the same
// reason: renderMs is ONE thread's wall time and is directly comparable to
// frameMs; a worker-pool total is not, and none is reported here.
//
// ===========================================================================
// FAILING READINGS, BOTH WAYS
// ===========================================================================
//
//   no "Voxel frame phase" line, with -VoxelFramePhase=1
//                       -> the hook was not applied. Nothing here ran.
//   frames=0            -> the hook is present but not called.
//   heavyN=0            -> THE LEG NEVER ENTERED THE REGIME BEING ASKED ABOUT.
//                          It says nothing about lifted caps and must not be
//                          read as evidence about them. Check the caps line;
//                          lower -VoxelFramePhaseHeavy only if the leg
//                          genuinely applies fewer chunks per frame than the
//                          threshold by design.
//   gameThreadMs=0.00 on every frame
//                       -> HARD ZERO. GGameThreadTime is not populated in this
//                          configuration and the GAME-THREAD HALF OF THE
//                          RECONCILIATION IS DEAD. It must NOT be read as "the
//                          game thread is idle" -- that is the same mistake as
//                          reading params=0.00ms as "the sampler is free".
//                          The residual absorbs it and will be enormous.
//   residual persistently NEGATIVE
//                       -> the engine globals describe a DIFFERENT frame than
//                          frameMs (they are set in FViewport::Draw and lag by
//                          one frame). Past a few ms the reconciliation is not
//                          valid at this granularity and NO verdict may be
//                          drawn from it.
//   heavy and light frame times EQUAL
//                       -> per-frame apply volume does not move frame time at
//                          all, and the ceiling is somewhere this file cannot
//                          see. Say that; do not pick a thread.
//
// AND THE WINDOW RULE: this reports per 5 s window and prints that window's own
// peak applies/frame, so a reader can say WHICH regime a line describes. Read
// the PEAK window and name it. The first window of a cold leg has falsified a
// real mechanism twice tonight, and the hitch line at the top of this file is
// the third.
#pragma once

#include "CoreMinimal.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace VoxelFramePhase
{
// -VoxelFramePhase=N, latched bitmask. 0 = OFF = this file does nothing at all
// and both hooks fold to a single predicted-not-taken branch on a latched int.
//
//   1  FRAME DISTRIBUTION, segmented at the cold-settle boundary. This is the
//      one GOAL 3 requires and it should be on for EVERY leg from here.
//   2  PHASE RECONCILIATION (the frame = tick + gameOther + gameWait + residual
//      breakdown above), for the lifted-cap 94 ms question.
//   3  both.
//
// Split rather than one flag because they answer different questions and cost
// different amounts: the distribution is a histogram increment per frame and is
// cheap enough to leave on permanently, while the reconciliation reads five
// engine globals and keeps six running sums per bucket.
inline int32 Mode()
{
	static const int32 Latched = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelFramePhase="), Value);
		return FMath::Max(0, Value);
	}();
	return Latched;
}

inline constexpr int32 kModeDistribution = 1;
inline constexpr int32 kModeReconcile    = 2;

// The out-of-line body. Never called with Mode() == 0.
void NoteFrameImpl(double VoxelTickMs, int32 AppliesThisFrame);

// HOOK 1, at the end of the streaming tick, with two values the caller has
// already computed.
FORCEINLINE void NoteFrame(double VoxelTickMs, int32 AppliesThisFrame)
{
	if (Mode() != 0)
	{
		NoteFrameImpl(VoxelTickMs, AppliesThisFrame);
	}
}

// HOOK 2, at the cold-settle SETTLED log site, which fires exactly once.
//
// THE BOUNDARY IS TOLD, NOT DERIVED, and that is the whole reason this hook
// exists rather than an apply-volume heuristic. "Derived, not verified,
// detaches" has produced five bugs on this project in three days, and a
// segment boundary guessed from throughput would silently mislabel the tail of
// the fill as settled -- which is precisely the blend GOAL 3 exists to undo.
//
// If it never fires, EVERY frame stays in FILL and the SETTLED segment reports
// n=0. The instrument says that loudly rather than printing fill numbers under
// a settled heading; see the failing readings above.
void NoteSettledImpl(double SettleSeconds);

FORCEINLINE void NoteSettled(double SettleSeconds)
{
	if (Mode() != 0)
	{
		NoteSettledImpl(SettleSeconds);
	}
}

} // namespace VoxelFramePhase
