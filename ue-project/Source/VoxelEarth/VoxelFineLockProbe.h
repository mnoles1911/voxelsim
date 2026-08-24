#pragma once
// VoxelFineLockProbe.h -- the instrument for FVoxelFineTileStreamer::Lock_, the
// one RWLock every terrain read in this process passes through.
//
// WHY THIS EXISTS
// ---------------
// `GetSurfaceHeightUU` is the most expensive per-chunk call in the streaming
// pipeline: four calls per chunk at two hot sites, each one a fine-tier
// `RequestFootprint` followed by a full `vxc::Amplifier::column`. Measured
// 2026-08-23: 9-15 us/call uncontended, and up to 183 us/call under load -- a
// 12-20x degradation that arrives exactly when throughput rises, which is the
// signature of a serialization point rather than of work.
//
// `RequestFootprint` takes `Lock_` EXCLUSIVELY, unconditionally, on every call
// (VoxelFineTileStreamer.cpp, and see the header's threading note). Meanwhile
// the meshing worker pool reads terrain through
// `FVoxelFineTileSamplerProxy::elevationMm`, which takes the SAME lock SHARED
// once per sampled pixel, from up to 36 threads at once. So the game thread
// raises a full pipeline barrier across the worker pool 4-8 times per chunk,
// and each barrier must first wait for every in-flight reader to drain.
//
// That is the hypothesis. NOBODY HAS MEASURED IT, and this file is the reason
// the fix that follows is allowed to be believed. It answers one question:
//
//     of the 183 us, how much is WAITING for the lock and how much is WORKING?
//
// THE FAILING READINGS, STATED AT THE SITE
// ----------------------------------------
// This project has found twelve instruments that read green because they were
// never in the path. The two failures that produce a zero here are DIFFERENT
// FAILURES and they must not print identically:
//
//   "the lock is never contended"  ->  entered>0, acq>0, wait totals ~0.
//       The probe ran, every acquire found the lock free, and the 183 us is
//       work rather than waiting. THIS FALSIFIES THE FIX -- see the disproof
//       condition in the streamer's RequestFootprint.
//   "the counter never ran"        ->  entered=0.
//       The probe is armed and no acquisition of any kind reached it. That is
//       an instrument outside the path, not an absence of contention, and the
//       report line says so in words ("PROBE ARMED BUT NEVER ENTERED").
//   "the probe is off"             ->  mode=0, and the line says `probe=off`.
//       Distinct from both of the above, and the default.
//
// COST, AND WHY THE COUNTERS ARE SHARDED PER THREAD
// -------------------------------------------------
// `elevationMm`'s shared acquire happens tens of millions of times a run from
// every worker. A single `std::atomic<uint64>` counter incremented from 36
// threads is itself a contended cacheline -- it would serialise the very
// traffic it is trying to measure and report a distorted number as fact. So
// every counter lives in a 64-byte-aligned per-thread shard, written only by
// the thread that owns it and summed on the game thread at report time. The
// residual distortion is one uncontended atomic add (and, at mode 2, two
// QPC reads) per acquire, which is stated in the report line rather than
// assumed away.
//
// MODES (-VoxelFineLockProbe=N)
//   0  OFF (default). No counters, no timing, no shard touched. The lock paths
//      are byte-identical to the pre-probe build.
//   1  TRAFFIC ONLY. Acquisition counts per site, shared vs exclusive. This is
//      the counter that comes BEFORE a timing one: it proves the instrument is
//      in the path and tells you the shape of the traffic, with no QPC on the
//      hot read. Safe to leave on for a throughput leg.
//   2  TRAFFIC + TIMING EXCEPT ON THE PER-PIXEL READS. This is the mode to
//      measure with. Wait and hold are timed at every EXCLUSIVE site and at the
//      low-rate shared ones, which is where GetSurfaceHeightUU's barrier lives
//      and therefore where the 183 us question is answered -- but elevationMm
//      and climate, which are taken millions of times a second from 36 threads,
//      are only COUNTED. Two QPC reads per acquire on paths that run thousands
//      of times a second, not millions.
//   3  TRAFFIC + TIMING EVERYWHERE, per-pixel reads included. This measures the
//      worker pool's own shared-acquire wait, which nothing else can -- and it
//      puts two QPC reads on the hottest call in the process, so it WILL depress
//      throughput. Never quote a chunks/s number from a mode-3 leg.
//
// WAIT AND HOLD ARE DIVIDED BY THE **TIMED** ACQUISITION COUNT, NOT THE TOTAL,
// which is what makes mode 2 readable: at mode 2 the shared side's `acq` is
// tens of millions and its `timed` is a few thousand, and dividing by the wrong
// one would report a per-acquire wait ~4 orders of magnitude too small. Both
// numbers are printed.

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeRWLock.h"

#include <atomic>

namespace VoxelFineLock
{

// Every place in FVoxelFineTileStreamer that touches Lock_. Split finely
// enough that "the game thread's prefetch is the barrier" and "the workers'
// own reads are the barrier" cannot be confused for one another -- they call
// for opposite fixes.
//
// ORDER IS LOAD-BEARING: everything from ReqExcl onwards is an EXCLUSIVE
// acquire, which is what SiteIsExclusive() tests. Insert shared sites before
// ReqShared and exclusive sites after TickExcl.
enum class ESite : uint8
{
	ElevShared = 0,  // FVoxelFineTileSamplerProxy::elevationMm -- the hot, many-threaded read
	ClimateShared,   // FVoxelFineTileSamplerProxy::climate
	FootprintShared, // IsFootprintResident -- the admission sweep, game thread
	BathyShared,     // ReadBathyRect -- long shared holds, off the game thread
	DiagShared,      // ResidentBytes/BudgetBytes/ResidentTileCount/RingCentre* -- the 5 s log
	ReqShared,       // RequestFootprint's all-resident fast path (-VoxelFineLockFast>=1)
	ReqExcl,         // RequestFootprint's load path -- the suspected barrier
	TickExcl,        // TickResidencyAndEviction -- the real I/O, once per streaming tick
	ColdGameExcl,    // ResolveNonResidentPixel, game thread: blocking load
	ColdWorkerExcl,  // ResolveNonResidentPixel, worker: leak report, no load
	Count
};

constexpr int32 kSiteCount = int32(ESite::Count);

inline bool SiteIsExclusive(ESite Site)
{
	return Site >= ESite::ReqExcl;
}

inline const TCHAR* SiteName(ESite Site)
{
	switch (Site)
	{
	case ESite::ElevShared: return TEXT("elev");
	case ESite::ClimateShared: return TEXT("climate");
	case ESite::FootprintShared: return TEXT("isResident");
	case ESite::BathyShared: return TEXT("bathy");
	case ESite::DiagShared: return TEXT("diag");
	case ESite::ReqShared: return TEXT("reqFast");
	case ESite::ReqExcl: return TEXT("reqExcl");
	case ESite::TickExcl: return TEXT("tick");
	case ESite::ColdGameExcl: return TEXT("coldGame");
	case ESite::ColdWorkerExcl: return TEXT("coldWorker");
	default: return TEXT("?");
	}
}

// --- the switches ------------------------------------------------------------
//
// Both latch on first call. FVoxelFineTileStreamer's constructor calls both, on
// the game thread, before any worker can reach them: FCommandLine::Get() is
// cheap but it is not something to first touch from inside a worker's hot read
// path, and a function-local static's guard variable is one more thing not to
// make a worker discover.

inline int32 ProbeMode()
{
	static const int32 Latched = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelFineLockProbe="), Value);
		return FMath::Clamp(Value, 0, 3);
	}();
	return Latched;
}

// The two sites taken once per SAMPLED PIXEL rather than once per call. Timing
// them costs two QPC reads on the hottest path in the process, so mode 2 leaves
// them counted-but-untimed and mode 3 times them anyway.
inline bool SiteIsPerPixel(ESite Site)
{
	return Site == ESite::ElevShared || Site == ESite::ClimateShared;
}

inline int32 FastMode()
{
	static const int32 Latched = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelFineLockFast="), Value);
		return FMath::Clamp(Value, 0, 3);
	}();
	return Latched;
}

// --- the counters ------------------------------------------------------------

// One cacheline-isolated block per thread. Written ONLY by its owning thread
// (relaxed atomics, so the game thread's summing read is defined rather than a
// data race); read only by Collect() on the game thread. A sum that races with
// a concurrent increment loses at most the in-flight increments of that
// instant, which for a 5 s window of millions is not a distinction anyone can
// act on -- and it buys the absence of a 36-way contended cacheline in the
// middle of the thing being measured.
struct alignas(64) FShard
{
	std::atomic<uint64> Acq[kSiteCount] = {};
	// Acquisitions whose wait/hold were actually measured. At mode 2 this is
	// far below Acq for the per-pixel sites, and it is the denominator every
	// per-acquire figure uses.
	std::atomic<uint64> TimedAcq[kSiteCount] = {};
	std::atomic<uint64> WaitNs[kSiteCount] = {};
	std::atomic<uint64> HoldNs[kSiteCount] = {};
	std::atomic<uint64> WaitMaxNs[kSiteCount] = {};
	std::atomic<uint64> HoldMaxNs[kSiteCount] = {};
	// The DISCRIMINATOR. Bumped on every acquisition the probe wrapper actually
	// ran, whatever the site and whatever the timings came out as. entered=0
	// with the probe armed means the instrument is not in the path; it does not
	// mean the lock is uncontended, and the report refuses to let those two
	// print the same.
	std::atomic<uint64> Entered = {0};
	uint8 Pad[56] = {};
};

// 64 shards, wrapped: the pool is 36 workers plus the game and render threads,
// so collisions are possible but not the common case, and a collision costs
// accuracy in exactly the way an unsharded counter costs it -- never
// correctness. Sized as a power of two so the index is a mask.
constexpr int32 kShards = 64;

inline FShard GShards[kShards];
inline std::atomic<uint32> GNextShard{0};

inline uint32 ShardIndex()
{
	static thread_local const uint32 Slot = GNextShard.fetch_add(1, std::memory_order_relaxed) & uint32(kShards - 1);
	return Slot;
}

inline void BumpMax(std::atomic<uint64>& Slot, uint64 Value)
{
	// Single-writer (this thread owns the shard), so a plain load/store pair is
	// the whole of the update -- no CAS loop, and nothing else can interleave.
	if (Value > Slot.load(std::memory_order_relaxed))
	{
		Slot.store(Value, std::memory_order_relaxed);
	}
}

// --- the RAII guard ----------------------------------------------------------
//
// Replaces FRWScopeLock at every Lock_ site. With the probe OFF this is
// exactly FRWScopeLock: one branch on a latched int, then the same
// ReadLock/WriteLock and the same unlock in the destructor.
class FLockScope
{
public:
	FLockScope(FRWLock& InLock, FRWScopeLockType InType, ESite InSite)
		: Lock(InLock)
		, Type(InType)
	{
		const int32 Mode = ProbeMode();
		if (Mode <= 0)
		{
			if (Type == SLT_Write) { Lock.WriteLock(); } else { Lock.ReadLock(); }
			return;
		}

		// mode 2 times everything but the per-pixel reads; mode 3 times those too.
		bTimed = (Mode >= 3) || (Mode >= 2 && !SiteIsPerPixel(InSite));
		Site = InSite;
		Shard = &GShards[ShardIndex()];

		const double T0 = bTimed ? FPlatformTime::Seconds() : 0.0;
		if (Type == SLT_Write) { Lock.WriteLock(); } else { Lock.ReadLock(); }

		Shard->Entered.fetch_add(1, std::memory_order_relaxed);
		Shard->Acq[int32(Site)].fetch_add(1, std::memory_order_relaxed);
		if (bTimed)
		{
			Shard->TimedAcq[int32(Site)].fetch_add(1, std::memory_order_relaxed);
			AcquiredAt = FPlatformTime::Seconds();
			const double WaitSec = AcquiredAt - T0;
			const uint64 WaitNs = uint64(FMath::Max(0.0, WaitSec) * 1.0e9);
			Shard->WaitNs[int32(Site)].fetch_add(WaitNs, std::memory_order_relaxed);
			BumpMax(Shard->WaitMaxNs[int32(Site)], WaitNs);
		}
	}

	~FLockScope()
	{
		if (bTimed)
		{
			// Measured BEFORE the release, so "hold" is the section and not the
			// section plus whatever the unlock has to wake up.
			const uint64 HoldNs = uint64(FMath::Max(0.0, FPlatformTime::Seconds() - AcquiredAt) * 1.0e9);
			Shard->HoldNs[int32(Site)].fetch_add(HoldNs, std::memory_order_relaxed);
			BumpMax(Shard->HoldMaxNs[int32(Site)], HoldNs);
		}
		if (Type == SLT_Write) { Lock.WriteUnlock(); } else { Lock.ReadUnlock(); }
	}

	FLockScope(const FLockScope&) = delete;
	FLockScope& operator=(const FLockScope&) = delete;

private:
	FRWLock& Lock;
	FRWScopeLockType Type;
	FShard* Shard = nullptr;
	double AcquiredAt = 0.0;
	ESite Site = ESite::ElevShared;
	bool bTimed = false;
};

// --- reporting ---------------------------------------------------------------

struct FAggregate
{
	uint64 Acq[kSiteCount] = {};
	uint64 TimedAcq[kSiteCount] = {};
	uint64 WaitNs[kSiteCount] = {};
	uint64 HoldNs[kSiteCount] = {};
	uint64 WaitMaxNs[kSiteCount] = {};
	uint64 HoldMaxNs[kSiteCount] = {};
	uint64 Entered = 0;

	uint64 AcqIn(bool bExclusive) const
	{
		uint64 Sum = 0;
		for (int32 I = 0; I < kSiteCount; ++I)
		{
			if (SiteIsExclusive(ESite(I)) == bExclusive) { Sum += Acq[I]; }
		}
		return Sum;
	}
	uint64 TimedAcqIn(bool bExclusive) const
	{
		uint64 Sum = 0;
		for (int32 I = 0; I < kSiteCount; ++I)
		{
			if (SiteIsExclusive(ESite(I)) == bExclusive) { Sum += TimedAcq[I]; }
		}
		return Sum;
	}
	uint64 WaitNsIn(bool bExclusive) const
	{
		uint64 Sum = 0;
		for (int32 I = 0; I < kSiteCount; ++I)
		{
			if (SiteIsExclusive(ESite(I)) == bExclusive) { Sum += WaitNs[I]; }
		}
		return Sum;
	}
	uint64 HoldNsIn(bool bExclusive) const
	{
		uint64 Sum = 0;
		for (int32 I = 0; I < kSiteCount; ++I)
		{
			if (SiteIsExclusive(ESite(I)) == bExclusive) { Sum += HoldNs[I]; }
		}
		return Sum;
	}
	uint64 MaxWaitNsIn(bool bExclusive) const
	{
		uint64 Max = 0;
		for (int32 I = 0; I < kSiteCount; ++I)
		{
			if (SiteIsExclusive(ESite(I)) == bExclusive && WaitMaxNs[I] > Max) { Max = WaitMaxNs[I]; }
		}
		return Max;
	}
	uint64 MaxHoldNsIn(bool bExclusive) const
	{
		uint64 Max = 0;
		for (int32 I = 0; I < kSiteCount; ++I)
		{
			if (SiteIsExclusive(ESite(I)) == bExclusive && HoldMaxNs[I] > Max) { Max = HoldMaxNs[I]; }
		}
		return Max;
	}
};

// Sums every shard. Game thread, once per report window.
inline FAggregate Collect()
{
	FAggregate Out;
	for (int32 S = 0; S < kShards; ++S)
	{
		const FShard& Shard = GShards[S];
		Out.Entered += Shard.Entered.load(std::memory_order_relaxed);
		for (int32 I = 0; I < kSiteCount; ++I)
		{
			Out.Acq[I] += Shard.Acq[I].load(std::memory_order_relaxed);
			Out.TimedAcq[I] += Shard.TimedAcq[I].load(std::memory_order_relaxed);
			Out.WaitNs[I] += Shard.WaitNs[I].load(std::memory_order_relaxed);
			Out.HoldNs[I] += Shard.HoldNs[I].load(std::memory_order_relaxed);
			const uint64 WMax = Shard.WaitMaxNs[I].load(std::memory_order_relaxed);
			if (WMax > Out.WaitMaxNs[I]) { Out.WaitMaxNs[I] = WMax; }
			const uint64 HMax = Shard.HoldMaxNs[I].load(std::memory_order_relaxed);
			if (HMax > Out.HoldMaxNs[I]) { Out.HoldMaxNs[I] = HMax; }
		}
	}
	return Out;
}

} // namespace VoxelFineLock
