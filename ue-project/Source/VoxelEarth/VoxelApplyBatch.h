// VoxelApplyBatch.h -- the apply path's per-chunk worldgen sampling, removed.
//
// ===========================================================================
// WHAT APPLY ACTUALLY SPENDS ITS TIME ON
// ===========================================================================
//
// Measured control arm, 2026-08-23 (docs/handoff-50k-2026-08-23-2200.md):
//
//     apply = 692 ms per 5 s window over ~12,700 drains = 54.5 us/chunk
//     of GAME THREAD. At the owner's 50,000 chunks/s that is 2.7 SECONDS of
//     game thread per second of gameplay. Apply alone makes 50k impossible.
//
// And on the same log line, `zeroQuad` EQUALS `drained` in every window on
// both arms: every chunk that drains meshes to zero quads. That is the
// marcher path -- geometry lives in the GPU brick pool, so a drained chunk
// creates no mesh, no UVoxelChunkComponent, no render proxy and no pool
// range. ApplyMeshResult's NumQuads == 0 branch is four cheap statements and
// a return (a weak-pointer get, a counter, ReleaseChunkGeometry over a record
// that holds nothing, a bool). It is not 54.5 us.
//
// THE COST IS NOT IN ApplyMeshResult AT ALL. It is in DrainResults, one line
// BELOW the apply call (VoxelWorldSubsystem.cpp, the tail of the drain loop):
//
//     VoxelBrickCpuArm::Publish(
//         Result.Key, Result.BrickPack,
//         ShadingFromChunkParams(SampleChunkParamsForPool(
//             Root, VoxelCoords::ChunkOriginWorldForLevel(...), Result.Key.Level)));
//
// SampleChunkParamsForPool is FOUR CORNER CALLS to
// UVoxelWorldSubsystem::GetSurfaceHeightUU plus a climate sample, per drained
// result. Each GetSurfaceHeightUU is a fine-tier RequestFootprint (takes the
// sampler lock, may touch disk) followed by a FULL amplifier column --
// vxc::Amplifier::column, with the cave lattice and the cavern passes -- on
// the GAME THREAD. The function's own comment prices one column-sample apply
// at 0.002-0.004 ms; task #44 took it to four corners, so 0.008-0.016 ms per
// drain, and that measurement predates both the fine tier and the 30 m -> 10 cm
// amplification redesign.
//
// TWO THINGS ARE WRONG WITH IT, and they are independent:
//
//   1. IT IS EVALUATED WHETHER OR NOT ANYTHING CONSUMES IT. C++ evaluates
//      arguments before the call, and Publish's FIRST statement is
//
//          if (!Pack.IsValid() || !VoxelGpuBrickPackResidentEnabled()) return;
//
//      Result.BrickPack is null on EVERY GPU-fork result (that arm publishes
//      at completion, not here) and on every result when the CPU arm is gated
//      off. So on the GPU-primary arm the entire sample -- four amplifier
//      columns and four footprint requests per chunk -- is computed and
//      thrown away. That is not a tuning question; it is work with no
//      consumer.
//
//   2. IT IS RECOMPUTED PER CHUNK FOR A QUANTITY THAT IS PER COLUMN. The four
//      corner heights, the climate bytes and the fitted gradients depend only
//      on (X, Y, Level). The chunk's Z enters exactly once, at the very end,
//      as a subtraction:  SurfaceZRelUU = BaseZUU - ChunkWorldOrigin.Z. A
//      level-0 band is 20-40 chunks deep, so the same four amplifier columns
//      are computed 20-40 times for one footprint.
//
// This header removes both, behind one latched switch, with traffic counters
// that make "shipped and inert" a readable failure instead of a silent one.
// See ELEVEN INERT FEATURES below.
//
// ===========================================================================
// WHY NO EXISTING COUNTER FOUND THIS -- AND WHY ONE OF THEM READS ALL-CLEAR
// ===========================================================================
//
// LevelZeroQuadMs / LevelQuadMs are WORKER-side. They sum Result.JobMs, CPU
// results only, and price what a worker spent producing a zero-quad chunk --
// the ceiling on a pre-dispatch buried-chunk skip. They say nothing about the
// game thread in either direction. LevelZeroQuadTotal is a count.
//
// `Voxel apply stages` IS named for this exact cost and is structurally blind
// to it. ApplyStageParamsMs's declaration reads "SampleChunkParamsForPool: a
// full Amplifier::column on the game thread", and its comment says that if the
// bucket is large, T1-3's column cache moves up the queue. But that bucket and
// AppliesTimedSinceLog are incremented ONLY inside ApplyMeshResult's POOLED
// branch -- and under voxel.Terrain.RetireQuads (the default) every chunk
// returns from the NumQuads == 0 branch ABOVE it. So every marcher leg prints
//
//     Voxel apply stages (5s window): ... timedApplies=0 params=0.00ms ...
//
// params=0.00ms is not "the sampler is free". It is "the branch this
// instrument watches was never taken". The sampler that DOES run is the one in
// DrainResults, which nothing wraps. That is why 54.5 us/chunk had no
// explanation for a night: the counter that would have named it was watching a
// different call site and reading a clean zero.
//
// ===========================================================================
// THE SWITCH -- -VoxelApplyFast=N  (bitmask, latched, default 0 = OFF)
// ===========================================================================
//
//   0  OFF. The default. ShadingForPublish below inlines to exactly the
//      expression it replaces -- one call to the sampler, one conversion, no
//      branch that survives constant folding, no counter, no clock read.
//      Byte-identical to today.
//   1  PUBLISH GUARD. Skip the sample entirely when the pack will not be
//      published (defect 1 above).
//   2  COLUMN CACHE. Memoise the per-(X,Y,Level) part of the sample (defect 2).
//   4  MEASURE ONLY. Time the sampler and report us/call, changing nothing
//      else. THIS IS THE FIRST LEG TO RUN: it settles whether the sampler is
//      really the 54.5 us before any behaviour changes. Implied by 1 and 2 so
//      that sampleUs is populated for the calls that still sample.
//
//   3 = guard + cache (the shipping candidate).  4 = measurement only.
//
//   -VoxelApplyColumnCache=N       column-cache slots, default 8192 (a
//                                  direct-mapped table, rounded up to a power
//                                  of two; 40 B/slot, so 8192 = 320 KB).
//   -VoxelApplyColumnCacheAudit=N  recompute and compare 1 hit in N. 0 = off.
//                                  Named *Audit and not *Verify on purpose:
//                                  VoxelFrontEndPolicy's naming rule treats a
//                                  switch containing "Verify" as SELF-DRIVING
//                                  and suppresses the front end for it. This
//                                  is tuning, and is registered as such in
//                                  tools/frontend-switch-classification.txt.
//
// ===========================================================================
// ELEVEN INERT FEATURES -- THE FAILING READINGS, STATED HERE
// ===========================================================================
//
// This project has shipped eleven features that did nothing while every
// indicator read healthy. A timing that goes down is NOT evidence this ran;
// a flat timing is not evidence it did not. So every arm carries traffic, and
// the readings that would condemn it are written down before the leg:
//
//   THE FAST PATH IS NEVER TAKEN
//     `Voxel apply fast` line absent from a leg launched with
//     -VoxelApplyFast=3            -> the hook was never applied to
//                                     DrainResults, or the module is not
//                                     linked. Nothing in this file ran.
//     line present, calls=0        -> the module is linked and latched but
//                                     nothing calls it: the hook is in a
//                                     branch the marcher path never reaches.
//     calls>0, guardSkip=0 cacheHit=0
//                                  -> the hook is live and BOTH arms are
//                                     inert. With mode=3 this is a FAIL, not
//                                     a null result. On the GPU-primary arm
//                                     guardSkip should be ~= calls; on the CPU
//                                     control arm guardSkip ~= 0 and cacheHit
//                                     should be ~= calls x (1 - 1/bandDepth),
//                                     i.e. 90-97%.
//     cacheHit=0 with cacheMiss ~= calls
//                                  -> the cache is enabled and every lookup
//                                     misses. cacheEvict ~= cacheMiss means
//                                     the table is thrashing (raise
//                                     -VoxelApplyColumnCache); cacheEvict ~= 0
//                                     means the KEY is wrong -- one entry per
//                                     chunk instead of one per column -- and
//                                     the cache is a pure loss.
//
//   THE FAST PATH IS TAKEN BUT WRONG
//     mismatch>0 under -VoxelApplyColumnCacheAudit=1
//                                  -> a cached shading differs from a fresh
//                                     sample of the same chunk. ANY non-zero
//                                     is a failure. The one mechanism that can
//                                     cause it and is not a coding error: a
//                                     fine tile became resident BETWEEN the
//                                     column's first chunk and a later one, so
//                                     the entry holds a pre-residency height.
//                                     maxMismatchUU says whether it is a
//                                     rounding tail (< 0.01 UU, see the ULP
//                                     note on FSlot::BaseZUU) or a real
//                                     wrong-ground event (metres).
//     sentinel>0                   -> HARD ZERO in a normal run. The sampler
//                                     returned "no surface gate", i.e. it found
//                                     no UWorld or no UVoxelWorldSubsystem, so
//                                     every brick published in that window
//                                     carries a DISABLED surface gate -- cave
//                                     floors tinted as turf, no error anywhere.
//                                     Upstream of this file, but this is where
//                                     it becomes visible.
//     offThread>0                  -> HARD ZERO. The table is unsynchronised
//                                     and the fine-tier prefetch is
//                                     game-thread-gated; a nonzero here is a
//                                     data race whose symptom is wrong terrain,
//                                     not a crash.
//     guardSkip>0 with withPack>0  -> not a failure of this change, but worth
//                                     acting on: the CPU arm PACKED those
//                                     chunks and the publication gate
//                                     (voxel.GPU.BrickPackResident) threw them
//                                     away. Worker time spent for nothing,
//                                     which "there was no producer" would have
//                                     hidden.
//     rootFlush climbing every window
//                                  -> the terrain root is MOVING, so the
//                                     cache is being flushed as fast as it
//                                     fills and its hit rate is meaningless.
//                                     Not a correctness failure (the flush is
//                                     what keeps it correct) but it voids the
//                                     cache arm of the leg.
//
//   THE DIAGNOSIS IS WRONG (the reading that condemns the whole change)
//     guardSkip + cacheHit large, `apply=` on the `Voxel tick budget` line
//     UNCHANGED                    -> the four amplifier columns were not
//                                     where apply's 54.5 us went. Revert;
//                                     do not tune. Take the -VoxelApplyFast=4
//                                     measurement leg first precisely so this
//                                     is known before anything changes.
//
//   IF THE WORLD LOOKS WRONG, BISECT WITH THE MODE, NOT WITH A COUNTER
//     With the guard OFF (mode=2, cache only) everything is sampled and
//     published exactly as control does, so mode=2 exercises the CACHE ALONE
//     and mode=3 adds the guard on top. mode=2 right + mode=3 wrong isolates
//     the guard; the reverse isolates the cache. This is the same-ground A/B
//     the owner has said settles these, and it does not depend on trusting any
//     number in this file.
//
#pragma once

#include "CoreMinimal.h"

#include "VoxelCoords.h"

#include "VoxelBrickPool.h"          // FVoxelBrickChunkShading, FVoxelBrickCpuPackRef
#include "VoxelGpuMeshJobManager.h"  // VoxelGpuBrickPackResidentEnabled()

#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

class USceneComponent;

namespace VoxelApplyFast
{
// --- mode bits ---------------------------------------------------------------
inline constexpr int32 kModeGuard   = 1; // skip the sample when nothing consumes it
inline constexpr int32 kModeCache   = 2; // memoise the per-column part of the sample
inline constexpr int32 kModeMeasure = 4; // time the sampler, change nothing

// LATCHED ONCE, command line only, for the reason -VoxelPendingJobCap and
// -VoxelGpuMesh are: -ExecCmds lands AFTER streaming has begun
// (tools/voxel-capture.ps1:114), so a cvar would flip apply half way through a
// cold fill and make the leg's number a blend of two behaviours. The A/B this
// exists to serve cannot be taken that way.
inline int32 Mode()
{
	static const int32 Latched = []
	{
		// DEFAULT 3 (guard+cache) AS OF 2026-08-23. -1 sentinel, not 0, so an
		// explicit -VoxelApplyFast=0 still selects the byte-identical control
		// arm -- the same rule -VoxelGpuBandSeedOnly uses, and without it the
		// control leg for this feature would be unrunnable.
		//
		// SHIPPED ON THIS EVIDENCE (q-audit.log, caps left at their defaults so
		// the arm isolates the amplifier fix):
		//   traffic   avoided/calls 79.6% -> 91.3% across the fill (not inert)
		//   G3        mismatch=0 sentinel=0 offThread=0 holes=0, and the
		//             cold-fill chunk population matched control to 1 chunk in
		//             164,732 with every hit audited (audit=902,154)
		//   headline  settle 23.7 s -> 21.8 s, 6,955 -> 7,547 chunks/s
		// Its disproof condition -- avoided ~= calls with apply= unchanged --
		// did not fire.
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelApplyFast="), Value);
		if (Value < 0) { Value = kModeGuard | kModeCache; }
		// Measurement is implied by either behavioural arm, so `sampleUs` is
		// populated for whatever sampling still happens and the before/after is
		// readable off ONE leg rather than two.
		if (Value & (kModeGuard | kModeCache)) { Value |= kModeMeasure; }
		return Value;
	}();
	return Latched;
}

// THE PREDICATE, ONE DEFINITION.
//
// This is character-for-character the test VoxelBrickCpuArm::Publish opens
// with. It is duplicated here rather than shared because Publish lives inside
// VoxelWorldSubsystem.cpp, which this change may not edit -- and a duplicated
// predicate is exactly the "join computed instead of checked" shape that has
// produced five bugs on this project in three days.
//
// NO COUNTER CAN CATCH THAT DRIFT FROM THIS SIDE, and pretending otherwise
// would be worse than saying so: this file cannot observe what Publish does
// with the value it is handed. What CAN catch it is the mode bisection -- a
// mode=2 leg (cache only, guard off) publishes exactly what control publishes,
// so "mode=2 right, mode=3 wrong" localises any divergence to this predicate in
// one A/B. That, and the fact that the guard's failure mode is loud in a
// screenshot (neutral climate, surface gate off: cave floors tinted as turf),
// is the whole defence.
//
// THE ONE-LINE FIX THAT REMOVES THE DUPLICATION ENTIRELY is for the lane
// holder to change Publish's early-out to
//     if (!VoxelApplyFast::WillPublish(Pack)) { return; }
// leaving one definition and two callers. It is listed as optional hook 2 in
// docs/apply-fast-path-2026-08-23.md.
inline bool WillPublish(const FVoxelBrickCpuPackRef& Pack)
{
	return Pack.IsValid() && VoxelGpuBrickPackResidentEnabled();
}

// The two file-static functions in VoxelWorldSubsystem.cpp, passed by address.
// BY ADDRESS AND NOT RE-IMPLEMENTED: ShadingFromChunkParams is a byte-exact
// round trip of a wire format shared with VoxelQuadVertexFactory.ush, and "a
// second transcription of a byte format is the defect shape this project has
// paid for most often" (VoxelBrickCpuArm::FinishPack). There is one
// definition; this file borrows it.
using FSampleParamsFn = FVector4f (*)(const USceneComponent&, const FVector&, int32);
using FShadingFromFn  = FVoxelBrickChunkShading (*)(const FVector4f&);

// The out-of-line implementation. Never called with Mode() == 0.
FVoxelBrickChunkShading ShadingForPublishSlow(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                              const FVoxelBrickCpuPackRef& Pack,
                                              const USceneComponent& Root,
                                              FSampleParamsFn SampleParams,
                                              FShadingFromFn ShadingFrom);

// The dispatch site's out-of-line entry point: same cache, guard unreachable.
// See its definition for the reqHdr measurement that justified wiring it.
FVoxelBrickChunkShading ShadingForDispatchSlow(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                               const USceneComponent& Root,
                                               FSampleParamsFn SampleParams,
                                               FShadingFromFn ShadingFrom);

// THE ONE EXPRESSION THE HOOK REPLACES.
//
// Returns what VoxelBrickCpuArm::Publish should be handed for this result. The
// caller passes the same key and the same pack it passes Publish, so the guard
// can decide before anything is sampled.
//
// OFF IS FREE, AND THAT IS WHY THE DISPATCH IS HERE AND NOT IN THE .cpp.
// Mode() is a function-local static latched once; with the switch absent the
// compiler folds this to `ShadingFrom(SampleParams(Root, Origin, Level))` with
// both pointers known at the call site. No branch survives, no counter moves,
// no clock is read. The control arm is byte-identical to today, which is the
// project's requirement for every switch and the reason the A/B is worth
// taking at all.
FORCEINLINE FVoxelBrickChunkShading ShadingForPublish(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                                      const FVoxelBrickCpuPackRef& Pack,
                                                      const USceneComponent& Root,
                                                      FSampleParamsFn SampleParams,
                                                      FShadingFromFn ShadingFrom)
{
	if (Mode() == 0)
	{
		return ShadingFrom(SampleParams(
			Root, VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level), Key.Level));
	}
	return ShadingForPublishSlow(Key, Pack, Root, SampleParams, ShadingFrom);
}

// Dispatch-site twin of ShadingForPublish, and OFF IS FREE HERE FOR THE SAME
// REASON: with the switch absent this folds to the exact expression it
// replaced, so no branch survives, no counter moves, and a control leg prints
// no 'Voxel apply fast' line at all. There is no pack at this site and the
// result is always consumed, so there is no guard arm to select.
FORCEINLINE FVoxelBrickChunkShading ShadingForDispatch(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                                       const USceneComponent& Root,
                                                       FSampleParamsFn SampleParams,
                                                       FShadingFromFn ShadingFrom)
{
	if (Mode() == 0)
	{
		return ShadingFrom(SampleParams(
			Root, VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level), Key.Level));
	}
	return ShadingForDispatchSlow(Key, Root, SampleParams, ShadingFrom);
}

// Emit the 5 s window line now and reset it, whatever the clock says.
//
// OPTIONAL. ShadingForPublishSlow flushes on its own 5 s clock, so the module
// reports without any second hook. Call this from MaybeLogCounters if you want
// the window edges to line up exactly with `Voxel tick budget` and
// `Voxel job flow` -- worth doing before publishing a per-chunk number, since
// apply-us/chunk is (apply= from one line) / (drained= from another) and the
// two windows must be the same window for that division to mean anything.
void FlushStats(bool bForce);

} // namespace VoxelApplyFast
