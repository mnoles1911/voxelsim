// VoxelApplyBatch.cpp -- see VoxelApplyBatch.h for the measurement this exists
// to remove, the switch, and the failing readings for every counter below.

#include "VoxelApplyBatch.h"

#include "VoxelDebug.h" // LogVoxelPerf

#include "Components/SceneComponent.h"
#include "Engine/World.h"

namespace VoxelApplyFast
{
namespace
{

// --- sizing ------------------------------------------------------------------

int32 CacheSlots()
{
	static const int32 Latched = []
	{
		// 8192 slots x 40 B = 320 KB, and the working set it must hold is one
		// cascade's worth of FOOTPRINTS, not chunks: the measured settled
		// cascade is 43,328 chunks over roughly 1,400 footprints per level, so
		// a few thousand slots cover the live set with room for the ring the
		// camera is walking into. Direct-mapped, so oversizing costs only
		// memory -- and cacheEvict is the counter that says whether it is
		// undersized.
		int32 Value = 8192;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelApplyColumnCache="), Value);
		Value = FMath::Clamp(Value, 64, 1 << 20);
		// Rounded UP to a power of two so the index is a mask, not a modulo:
		// this runs once per drained chunk, and a 64-bit division here would be
		// a measurable fraction of exactly what the change is trying to reclaim.
		return int32(FMath::RoundUpToPowerOfTwo(uint32(Value)));
	}();
	return Latched;
}

int32 AuditEveryN()
{
	static const int32 Latched = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelApplyColumnCacheAudit="), Value);
		return FMath::Max(0, Value);
	}();
	return Latched;
}

// --- the table ---------------------------------------------------------------

// PER (X, Y, LEVEL), NOT PER CHUNK, and that is the whole mechanism.
// SampleChunkParamsForPool's four corner heights, its climate bytes and its
// fitted gradients are functions of the chunk's XY footprint and its level
// alone. The chunk's Z enters exactly once, at the very end, as
//     SurfaceZRelUU = BaseZUU - ChunkWorldOrigin.Z
// so a whole vertical band shares one entry and differs only by a subtraction.
struct FSlot
{
	int32 Level = -1; // -1 = empty
	int32 X = 0;
	int32 Y = 0;
	float Temperature = 0.f;   // FVector4f.X exactly as the sampler produced it
	float Precipitation = 0.f; // FVector4f.Y
	float GradPacked = 0.f;    // FVector4f.W
	// THE FITTED PLANE'S ABSOLUTE WORLD Z, reconstructed once at fill time as
	// double(Params.Z) + ChunkWorldOrigin.Z.
	//
	// THE ROUND TRIP IS NOT EXACT AND ITS ERROR IS BOUNDED HERE RATHER THAN
	// LEFT TO BE DISCOVERED. Params.Z is float32 and already chunk-relative, so
	// reconstructing the absolute base costs one float32 rounding of a value
	// whose magnitude is at most a few thousand UU: 0.0005 UU at 8192 UU,
	// against a 10 UU voxel -- 5e-5 of a voxel. Every chunk that then HITS this
	// entry inherits that single rounding and nothing compounds, because the
	// stored double is written once. The ABSOLUTE height (~8.4e6 UU) is never
	// rounded to float, which is the precision trap SampleChunkParamsForPool
	// documents and deliberately avoids; this preserves that property.
	//
	// -VoxelApplyColumnCacheAudit reports maxMismatchUU, so the tail is a
	// measured number in the log rather than this comment's claim.
	double BaseZUU = 0.0;
};

TArray<FSlot>& Table()
{
	static TArray<FSlot> Slots;
	if (Slots.Num() == 0)
	{
		Slots.SetNum(CacheSlots());
	}
	return Slots;
}

// FNV-ish fold of the three key fields. Collisions are not a correctness
// question here: the slot stores X/Y/Level and they are compared EXACTLY, so a
// collision is a miss and an eviction, never a wrong answer.
FORCEINLINE uint32 SlotIndex(int32 Level, int32 X, int32 Y, uint32 Mask)
{
	uint64 H = 1469598103934665603ull;
	H = (H ^ uint64(uint32(X))) * 1099511628211ull;
	H = (H ^ uint64(uint32(Y))) * 1099511628211ull;
	H = (H ^ uint64(uint32(Level))) * 1099511628211ull;
	return uint32(H ^ (H >> 32)) & Mask;
}

// --- invalidation ------------------------------------------------------------
//
// The cached corner heights were sampled at
//     Root.GetComponentLocation() + ChunkOriginWorldForLevel(Key)
// so they are valid only for the Root transform they were taken under. The
// terrain root is static in every configuration this project ships -- but
// "static in practice" is precisely the kind of assumption this codebase has
// paid for repeatedly, so it is CHECKED once per call with an exact FVector
// compare and the whole table is dropped if it ever moves. rootFlush is the
// counter; a nonzero one voids the cache arm of a leg rather than corrupting
// it. The world pointer is compared for the same reason, across a PIE restart
// inside one process.
FVector CachedRootLoc = FVector::ZeroVector;
const UWorld* CachedWorld = nullptr;
bool bCacheAnchored = false;

// --- counters (one 5 s window) ----------------------------------------------
//
// Every one of these has its failing reading written down in the header. They
// are TRAFFIC, not timing: this project has shipped eleven features that were
// inert while every indicator read healthy, and a timing that improves is not
// evidence that a particular line of code ran.
int64 Calls = 0;            // ShadingForPublish calls that reached the slow path
int64 Sampled = 0;          // ...of which actually ran SampleChunkParamsForPool
int64 GuardSkipped = 0;     // ...of which returned without sampling (nothing consumes it)
int64 GuardSkipWithPack = 0; // ...of THOSE that DID hold a valid pack (see below)
int64 CacheHits = 0;
int64 CacheMisses = 0;
int64 CacheEvicts = 0;      // a miss that overwrote a DIFFERENT live entry
int64 SentinelUncached = 0; // "no surface gate" results, never cached (see below)
int64 RootFlushes = 0;
int64 OffThread = 0;        // HARD ZERO: this path is game-thread only
int64 AuditsRun = 0;
int64 AuditMismatches = 0;
double MaxMismatchUU = 0.0;
double SampleSeconds = 0.0; // wall time inside SampleChunkParamsForPool
int64 AuditTick = 0;

double LastLogSeconds = 0.0;

// 0.01 UU = 1/1000 of a voxel, and two orders of magnitude above the 0.0005 UU
// float32 reconstruction tail documented on FSlot::BaseZUU. A mismatch that
// clears this is a wrong GROUND height, not a rounding artifact -- the fine
// tile became resident between the column's first chunk and this one, and the
// entry is holding a pre-residency answer. maxMismatchUU is reported whether or
// not the tolerance is crossed, which is what separates the two cases without
// needing a rebuild.
constexpr double kAuditToleranceUU = 0.01;

// UVoxelGpuPoolComponent::kNoSurfaceGate is -1.0e30f: "no subsystem, no gate,
// always surface". It is a SENTINEL, not a height, and running it through the
// absolute-base reconstruction would turn it into a merely-very-negative number
// that no longer compares equal to the sentinel a consumer tests for. So a
// sentinel result is returned untouched and never enters the table.
constexpr float kSurfaceSentinelCeiling = -1.0e29f;

} // namespace

void FlushStats(bool bForce)
{
	const double Now = FPlatformTime::Seconds();
	if (LastLogSeconds <= 0.0)
	{
		LastLogSeconds = Now; // first call: start the window, print nothing
		return;
	}
	if (!bForce && Now - LastLogSeconds < 5.0)
	{
		return;
	}
	if (Mode() == 0)
	{
		return; // control arm: this module prints nothing at all
	}

	const int32 M = Mode();
	const double WindowSec = FMath::Max(1e-6, Now - LastLogSeconds);
	const double SampleUsPerSample = Sampled > 0 ? (SampleSeconds * 1e6) / double(Sampled) : 0.0;
	const double SampleUsPerCall = Calls > 0 ? (SampleSeconds * 1e6) / double(Calls) : 0.0;
	const int64 Avoided = GuardSkipped + CacheHits;

	// READ `avoided` AND `apply=` TOGETHER, NEVER EITHER ALONE.
	//
	//   avoided ~= calls, apply= down     -> the change did what it claims.
	//   avoided ~= calls, apply= FLAT     -> the sampler was not apply's cost.
	//                                        The diagnosis is wrong: revert, do
	//                                        not tune. Take the -VoxelApplyFast=4
	//                                        leg first so this is known before
	//                                        any behaviour changes.
	//   avoided = 0                       -> nothing ran. FAIL, not a null
	//                                        result, whatever apply= says.
	//   avoided ~= calls, mismatch > 0    -> it ran and it is wrong.
	//
	// sampleUs/call is the DIRECT price of the four amplifier columns per
	// drained chunk. At mode=4 (measure only, behaviour unchanged) it is the
	// entire claim of this change, measured, before anything is altered.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel apply fast (5s window): mode=%d(%s%s%s) calls=%lld avoided=%lld (%.1f%%) ")
	       TEXT("| guardSkip=%lld (withPack=%lld) cacheHit=%lld cacheMiss=%lld cacheEvict=%lld ")
	       TEXT("sentinel=%lld | sampled=%lld sampleUs/sample=%.2f sampleUs/call=%.2f ")
	       TEXT("sampleMsWindow=%.1f | audit=%lld mismatch=%lld maxMismatchUU=%.4f ")
	       TEXT("| rootFlush=%lld offThread=%lld slots=%d win=%.1fs"),
	       M,
	       (M & kModeGuard) ? TEXT("guard") : TEXT("-"),
	       (M & kModeCache) ? TEXT("+cache") : TEXT(""),
	       (M & kModeMeasure) ? TEXT("+measure") : TEXT(""),
	       (long long)Calls, (long long)Avoided,
	       Calls > 0 ? 100.0 * double(Avoided) / double(Calls) : 0.0,
	       (long long)GuardSkipped, (long long)GuardSkipWithPack,
	       (long long)CacheHits, (long long)CacheMisses, (long long)CacheEvicts,
	       (long long)SentinelUncached,
	       (long long)Sampled, SampleUsPerSample, SampleUsPerCall, SampleSeconds * 1000.0,
	       (long long)AuditsRun, (long long)AuditMismatches, MaxMismatchUU,
	       (long long)RootFlushes, (long long)OffThread,
	       CacheSlots(), WindowSec);

	Calls = Sampled = GuardSkipped = GuardSkipWithPack = 0;
	CacheHits = CacheMisses = CacheEvicts = SentinelUncached = 0;
	RootFlushes = OffThread = 0;
	AuditsRun = AuditMismatches = 0;
	MaxMismatchUU = 0.0;
	SampleSeconds = 0.0;
	LastLogSeconds = Now;
}

FVoxelBrickChunkShading ShadingForPublishSlow(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                              const FVoxelBrickCpuPackRef& Pack,
                                              const USceneComponent& Root,
                                              FSampleParamsFn SampleParams,
                                              FShadingFromFn ShadingFrom)
{
	const int32 M = Mode();
	const FVector OriginRelative = VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level);

	// GAME THREAD ONLY. The table is unsynchronised, and GetSurfaceHeightUU's
	// own fine-tier prefetch is game-thread-gated for the same reason (it takes
	// the sampler's lock exclusively and can do disk I/O). Both call sites are
	// game-thread, so offThread is a hard zero -- but a data race here would
	// present as wrong terrain rather than a crash, so it is checked instead of
	// asserted in a comment.
	if (!IsInGameThread())
	{
		++OffThread;
		return ShadingFrom(SampleParams(Root, OriginRelative, Key.Level));
	}

	++Calls;
	// The window flush costs one clock read; amortised over 256 drained chunks
	// it is free, and at 12,700 drains per 5 s window the log still lands within
	// ~0.1 s of its nominal edge. Pass -- to FlushStats from MaybeLogCounters
	// (optional hook 3) if exact alignment with `Voxel tick budget` is wanted.
	if ((Calls & 255) == 0)
	{
		FlushStats(/*bForce*/ false);
	}

	auto Sample = [&]() -> FVector4f
	{
		++Sampled;
		if (M & kModeMeasure)
		{
			// Two FPlatformTime::Seconds() around a call claimed to cost ~10 us.
			// The pair costs tens of nanoseconds, so it is well under 1% of what
			// it measures -- and it is STILL gated, because "an instrument that
			// becomes what it measures" is a recorded failure on this exact path
			// (voxel.Stream.ApplyStageStats carries the same warning) and a
			// shipping default must not carry one.
			const double T0 = FPlatformTime::Seconds();
			const FVector4f P = SampleParams(Root, OriginRelative, Key.Level);
			SampleSeconds += FPlatformTime::Seconds() - T0;
			return P;
		}
		return SampleParams(Root, OriginRelative, Key.Level);
	};

	// --- ARM 1: THE PUBLISH GUARD --------------------------------------------
	//
	// Nothing consumes the result, so nothing is computed. This is the arm that
	// matters on the GPU-primary configuration, where Result.BrickPack is null
	// for EVERY drained result (the fork publishes its bricks at completion),
	// and the four amplifier columns were being computed and discarded 12,700
	// times per 5 s window.
	if ((M & kModeGuard) && !WillPublish(Pack))
	{
		++GuardSkipped;
		if (Pack.IsValid())
		{
			// The pack EXISTS and only the publication gate
			// (voxel.GPU.BrickPackResident) refused it. Returning neutral is
			// still correct -- Publish refuses on the same test -- but a nonzero
			// here means the CPU arm is packing chunks nobody stores, which is
			// worker time being spent for nothing and is worth knowing about
			// separately from "there was no producer".
			++GuardSkipWithPack;
		}
		// Neutral, not garbage: if this value were ever consumed despite the
		// guard, the failure is "mid-range climate, surface gate off" -- visible
		// and diagnosable -- rather than an uninitialised plane.
		return FVoxelBrickChunkShading::Neutral();
	}

	// --- ARM 2: THE PER-COLUMN CACHE -----------------------------------------
	//
	// Note the ordering: with the guard OFF (mode=2 or 4) everything is sampled
	// and published exactly as control does, so mode=2 exercises the CACHE
	// ALONE. That is the bisection if a mode=3 leg looks wrong in the world:
	// mode=2 right and mode=3 wrong isolates the guard, the reverse isolates the
	// cache, and neither needs a counter to be trusted.
	if ((M & kModeCache) == 0)
	{
		return ShadingFrom(Sample());
	}

	// Anchor check (see CachedRootLoc). Exact compare, no tolerance: a moved
	// root means every cached corner height was sampled at the wrong XY, and
	// "nearly the same place" is not a defensible basis for keeping them.
	const FVector RootLoc = Root.GetComponentLocation();
	const UWorld* World = Root.GetWorld();
	if (!bCacheAnchored || RootLoc != CachedRootLoc || World != CachedWorld)
	{
		if (bCacheAnchored)
		{
			++RootFlushes;
		}
		for (FSlot& S : Table())
		{
			S.Level = -1;
		}
		CachedRootLoc = RootLoc;
		CachedWorld = World;
		bCacheAnchored = true;
	}

	const double ChunkWorldZ = RootLoc.Z + OriginRelative.Z;
	const uint32 Mask = uint32(CacheSlots() - 1);
	FSlot& Slot = Table()[SlotIndex(Key.Level, Key.Key.X, Key.Key.Y, Mask)];

	if (Slot.Level == Key.Level && Slot.X == Key.Key.X && Slot.Y == Key.Key.Y)
	{
		++CacheHits;

		FVector4f Cached;
		Cached.X = Slot.Temperature;
		Cached.Y = Slot.Precipitation;
		Cached.Z = float(Slot.BaseZUU - ChunkWorldZ); // the ONLY per-chunk term
		Cached.W = Slot.GradPacked;

		const int32 AuditN = AuditEveryN();
		if (AuditN > 0 && (++AuditTick % int64(AuditN)) == 0)
		{
			++AuditsRun;
			const FVector4f Fresh = Sample();
			const double DZ = FMath::Abs(double(Fresh.Z) - double(Cached.Z));
			MaxMismatchUU = FMath::Max(MaxMismatchUU, DZ);
			const bool bOtherDiffers =
				(Fresh.X != Cached.X) || (Fresh.Y != Cached.Y) || (Fresh.W != Cached.W);
			if (bOtherDiffers || DZ > kAuditToleranceUU)
			{
				++AuditMismatches;
				UE_LOG(LogVoxelPerf, Error,
				       TEXT("Voxel apply fast AUDIT MISMATCH: L%d (%d,%d,%d) cached ")
				       TEXT("(T=%.5f P=%.5f Z=%.4f G=%.1f) fresh (T=%.5f P=%.5f Z=%.4f G=%.1f) ")
				       TEXT("dz=%.4f UU"),
				       Key.Level, Key.Key.X, Key.Key.Y, Key.Key.Z,
				       Cached.X, Cached.Y, Cached.Z, Cached.W,
				       Fresh.X, Fresh.Y, Fresh.Z, Fresh.W, DZ);
			}
			// THE FRESH VALUE WINS. Auditing must never be able to make a run
			// worse than control: an audited leg publishes exactly what the
			// uncached path would have published, and only the counters differ.
			return ShadingFrom(Fresh);
		}

		return ShadingFrom(Cached);
	}

	++CacheMisses;
	if (Slot.Level >= 0)
	{
		// A live entry for a DIFFERENT column is being overwritten. cacheEvict
		// ~= cacheMiss with a low hit rate is the table thrashing and wants
		// -VoxelApplyColumnCache raised; cacheEvict ~= 0 with a low hit rate
		// means the KEY is wrong, which is a completely different problem.
		++CacheEvicts;
	}

	const FVector4f Fresh = Sample();

	if (Fresh.Z > kSurfaceSentinelCeiling)
	{
		Slot.Level = Key.Level;
		Slot.X = Key.Key.X;
		Slot.Y = Key.Key.Y;
		Slot.Temperature = Fresh.X;
		Slot.Precipitation = Fresh.Y;
		Slot.GradPacked = Fresh.W;
		Slot.BaseZUU = double(Fresh.Z) + ChunkWorldZ;
	}
	else
	{
		// "No surface gate" sentinel -- see kSurfaceSentinelCeiling. Not stored,
		// and the slot is left as it was rather than half-filled.
		//
		// EXPECTED HARD ZERO in a normal run: it means SampleChunkParamsForPool
		// found no UWorld or no UVoxelWorldSubsystem, so every brick published
		// in that window carries a DISABLED surface gate -- cave floors tinted
		// as turf, with no error anywhere. If this counter moves, the problem is
		// upstream of this file.
		++SentinelUncached;
	}

	return ShadingFrom(Fresh);
}

} // namespace VoxelApplyFast

// ---------------------------------------------------------------------------
// THE PER-TICK CEILINGS. See the block above these declarations in
// VoxelApplyBatch.h for the ahead-on.log table that found them, the order they
// bind in, and the failing readings both ways.
//
// All three are LATCHED and all three default to "whatever the caller already
// had". With no switch on the command line every one of these returns its
// argument (or 1024) and the drain loop is byte-identical to today.
// ---------------------------------------------------------------------------

namespace VoxelApplyFast
{
namespace
{
constexpr int32 kShippedDrainCap = 1024; // the constexpr this replaces

// ONE LINE, ONCE, NAMING THE EFFECTIVE VALUES.
//
// Without it, "the switch is on" and "the switch did not parse" are the same
// log. That is the eleven-inert-features failure in its purest form, and it is
// cheaper to print three numbers than to run a leg that cannot be interpreted.
// Printed from the first cap query of the session, which is inside DrainResults
// on the game thread.
void LogCapsOnce(int32 EffApplies, double EffBudgetSec, int32 EffDrains,
                 int32 CvarApplies, double CvarBudgetSec)
{
	static bool bLogged = false;
	if (bLogged)
	{
		return;
	}
	bLogged = true;
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel apply caps: maxApplies=%d (cvar %d) budgetMs=%.2f (cvar %.2f) drainCap=%d (shipped %d)")
	       TEXT(" -- read `Voxel apply stages` exit= for which one binds; queueEmpty>0 during a FILLING")
	       TEXT(" window means apply has caught up and is no longer the bound."),
	       EffApplies, CvarApplies, EffBudgetSec * 1000.0, CvarBudgetSec * 1000.0,
	       EffDrains, kShippedDrainCap);
}

int32 AppliesOverride()
{
	static const int32 Latched = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelApplyCap="), Value);
		return FMath::Max(0, Value); // 0 = use the cvar
	}();
	return Latched;
}

float BudgetMsOverride()
{
	static const float Latched = []
	{
		float Value = -1.f;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelApplyBudgetMs="), Value);
		return Value; // < 0 = use the cvar
	}();
	return Latched;
}

int32 DrainOverride()
{
	static const int32 Latched = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelApplyDrainCap="), Value);
		return FMath::Max(0, Value); // 0 = the shipped constant
	}();
	return Latched;
}
} // namespace

// The pair AppliesPerTickCap last resolved, so ApplyBudgetSeconds -- called
// three lines later on the same tick, always after it -- can print one complete
// line instead of two half ones. Game thread only, like everything here.
static int32 LastCvarApplies = 0;
static int32 LastEffApplies = 0;

int32 AppliesPerTickCap(int32 CvarValue)
{
	const int32 Override = AppliesOverride();
	LastCvarApplies = CvarValue;
	LastEffApplies = Override > 0 ? Override : CvarValue;
	return LastEffApplies;
}

double ApplyBudgetSeconds(double CvarSeconds)
{
	const float OverrideMs = BudgetMsOverride();
	const double Effective = OverrideMs >= 0.f ? double(OverrideMs) / 1000.0 : CvarSeconds;
	// First point in a tick where all three effective values are known.
	LogCapsOnce(LastEffApplies, Effective, DrainsPerTickCap(), LastCvarApplies, CvarSeconds);
	return Effective;
}

int32 DrainsPerTickCap()
{
	const int32 Override = DrainOverride();
	return Override > 0 ? Override : kShippedDrainCap;
}

} // namespace VoxelApplyFast
