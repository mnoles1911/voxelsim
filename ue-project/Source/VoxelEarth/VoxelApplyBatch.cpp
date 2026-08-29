// VoxelApplyBatch.cpp -- see VoxelApplyBatch.h for the measurement this exists
// to remove, the switch, and the failing readings for every counter below.

#include "VoxelApplyBatch.h"

#include "VoxelDebug.h" // LogVoxelPerf

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h" // voxel.Stream.ShadingCacheWays

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
		// 8192 -> 131072 (2026-08-23). At 8192 the table THRASHED: cacheEvict
		// was 96% of cacheMiss, which is this counter's documented "undersized"
		// reading rather than its "wrong key" one (evict ~= 0 would be the
		// latter). Matched legs, one switch, mode 3, shipped caps:
		//
		//   slots     avoided    hit rate   evict/miss   sampler misses   settle
		//     8192      89.1%      81.5%          96%          204,243    23.4 s
		//   131072      91.9%      86.1%          43%          155,401    22.3 s
		//
		// 24% fewer calls into the fine-tier sampler lock, and avoided/calls
		// crosses the 90% the apply-fast brief set as its traffic gate. Cost is
		// 131,072 x 40 B = 5.2 MB of a direct-mapped table, allocated once.
		// evict/miss at 43% says the knee is at or above this; it is not below.
		int32 Value = 131072;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelApplyColumnCache="), Value);
		Value = FMath::Clamp(Value, 64, 1 << 20);
		// Rounded UP to a power of two so the index is a mask, not a modulo:
		// this runs once per drained chunk, and a 64-bit division here would be
		// a measurable fraction of exactly what the change is trying to reclaim.
		return int32(FMath::RoundUpToPowerOfTwo(uint32(Value)));
	}();
	return Latched;
}

// --- THE SUCCESSOR TO THE WARM FAMILY, AND IT IS TABLE SHAPE -----------------
//
// THREE WARMERS DIED AGAINST THIS TABLE (docs/warmshadingasync-null-2026-08-29
// .md; SCOREBOARD 2026-08-29). WarmShadingAhead inline, WarmShadingAsync on
// workers, and the cold-shading cap each passed every mechanism gate they set
// themselves -- launched == drained == filled, no gate leak, holes clean -- and
// each was NULL on its verdict counter. The level census (coldByLevel, 4fdd5db)
// then said why the "the walk and demand are disjoint" reading was wrong: the
// colds sit at L0 ~55%, L1 ~25%, L2 ~12% -- the levels the walk already targets
// -- and on the same window line
//
//     cacheEvict ~370-490 per window against ~800-1,000 cold samples
//
// Collision evictions run at HALF the cold rate. This table has always been
// DIRECT-MAPPED, so a large share of demand's colds are RE-SAMPLES of ground
// that was warm and got recycled by a hash collision. No predictive walk can
// pre-fill a churn population: demand's set is substantially the walk's OWN
// PAST FILLS, evicted. Nobody should build a fourth warmer.
//
// IT IS LOAD FACTOR, NOT ALIASING, AND THE ARITHMETIC SAYS SO. CacheSlots'
// comment above prices the working set at "roughly 1,400 footprints per level"
// -- 9,800 keys over 7 levels, a load factor of 0.075, at which a direct-mapped
// table would evict on ~7% of misses, not 43%. That estimate is STALE: it was
// written for the six-level world, and the cascade holds Outer/ChunkEdge == 40
// at every level (VoxelStreamAdmission::AdmitOuterUU), so ONE level's admitted
// disc alone is pi*40^2 ~= 5,000 distinct XY footprints and the resident
// (evict) radius is wider still. Take the file's own measured evict/miss of 43%
// at 131,072 slots and invert the occupancy 1 - exp(-n/m): n ~= 74,000 live
// keys, a load factor of 0.56. The table is not 15x oversized for its working
// set; it is a little over half full, which is exactly where a direct-mapped
// table starts throwing live entries away.
//
// THE ARM: voxel.Stream.ShadingCacheWays=2. Each set holds two entries; a miss
// takes a free way when the set has one, and otherwise evicts the LESS RECENTLY
// TOUCHED of the two rather than whichever key happened to hash there. The SET
// COUNT does not move, so the counterfactual is exact for a given key stream: a
// miss into a set holding exactly one live entry was an eviction at 1 way and
// is not one at 2. That is counted directly, as evictSpared=.
//
// WHY 2 WAYS AND NOT A BIGGER TABLE, in the same model. At n/m = 0.56:
//
//     arm                          entries    memory   predicted evict/miss
//     1 way, 131,072 sets (today)  131,072    4.0 MiB   43%  (the measurement)
//     1 way, 262,144 sets          262,144    8.0 MiB   25%
//     2 ways, 131,072 sets (ARM)   262,144    9.0 MiB   11%
//
// Associativity is worth ~2.2x more than the extra sets at the same entry
// count, because a second way ABSORBS the collisions that more sets only
// dilute. The model is CALIBRATED on the 1-way reading and then PREDICTS the
// 2-way one, so "cacheEvict lands near 11% of cacheMiss" is a real number this
// arm can miss. A 2-way set is also 2 x 32 B = one cache line's worth, so
// probing the second way is usually not a second fetch.
//
// PRE-REGISTERED GATES. ANY ONE moving the wrong way refutes the arm and the
// default stays at 1 way:
//   * cacheEvict falls HARD *and* cacheMiss falls WITH IT on the submit
//     population. cacheEvict alone falling is NOT a result: a table that evicts
//     less while missing as often has not made a single sample cheaper.
//   * audit mismatch=0 under -VoxelApplyColumnCacheAudit=1. This arm changes
//     WHICH entry is kept and never what an entry means; a mismatch is a
//     way-indexing or a keying defect and voids the leg.
//   * maxReqHdr NOTED BUT NOT TRUSTED. It printed 22.08 to the digit on both
//     WarmShadingAsync legs and is suspected to be a lifetime max latched at
//     fill rather than a window max -- read its ++ site before quoting it.
//   * stutterPct, p95, p99 and flight holes NOT WORSE.
// ENGAGEMENT, and it cannot read inert: the window line prints ways= and slots=
// (the real ENTRY count, sets x ways), and hitWay2= counts demand hits served
// from the second way -- a hard zero at 1 way and impossible to fake at 2.
static TAutoConsoleVariable<int32> CVarVoxelStreamShadingCacheWays(
	TEXT("voxel.Stream.ShadingCacheWays"), 1,
	TEXT("Associativity of the per-column shading table. 1 = the shipped default and ")
	TEXT("byte-identical to the direct-mapped table this file has always had; 2 = the arm, ")
	TEXT("which keeps the same SET count and gives each set a second entry, evicting the ")
	TEXT("less recently touched of the two instead of whichever key hashed there. Exists ")
	TEXT("because the warm family closed on this table: cacheEvict ~400/window against ")
	TEXT("~900 cold samples means much of demand's cold set is its own past fills, recycled ")
	TEXT("by collision, and no warmer can pre-fill churn. INIT-LATCHED: the table is sized ")
	TEXT("once on the first drained chunk and a change afterwards is REFUSED with a one-shot ")
	TEXT("Warning naming the value the run is really measuring -- and because -ExecCmds ")
	TEXT("lands AFTER streaming has begun, a LEG must arm this on the command line as ")
	TEXT("-VoxelApplyColumnCacheWays=2, not through this cvar. COSTS, armed: 131,072 x 32 B ")
	TEXT("= 4.0 MiB of table becomes 262,144 x 32 B plus a 1.0 MiB LRU stamp array = 9.0 ")
	TEXT("MiB. ENGAGEMENT: ways= and hitWay2= on the `Voxel apply fast` line (hitWay2 is a ")
	TEXT("hard zero at 1 way). VERDICT: cacheEvict falling hard AND cacheMiss falling with ")
	TEXT("it on the submit population, under audit mismatch=0, with stutterPct/p95/p99/holes ")
	TEXT("not worse. Predicted evict/miss 43%% -> ~11%%."),
	ECVF_Default);

// What the cvar read at the moment CacheWays() latched. Kept so the mid-run
// refusal below can tell a real late change from a leg that armed on the
// command line and left the cvar at its default -- warning about the latter
// would fire on every armed leg, which is how a warning stops being read.
int32 CvarWaysAtLatch = -1;

int32 CacheWays()
{
	static const int32 Latched = []
	{
		// COMMAND LINE OUTRANKS THE CVAR, AND THAT IS NOT A CONVENIENCE.
		// -ExecCmds lands AFTER streaming has begun (tools/voxel-capture.ps1
		// :114) -- the stated reason Mode(), CacheSlots() and AuditEveryN() are
		// command-line only -- and this table is sized on the first drained
		// chunk. A leg armed through the cvar alone would size a 1-way table and
		// then be told to be 2-way: an armed leg measuring the control, which is
		// the silent-success failure this module documents eleven times over.
		// The cvar exists so the arm sits beside its siblings in the console and
		// so an interactive session can set it before the first chunk drains.
		int32 Value = CVarVoxelStreamShadingCacheWays.GetValueOnGameThread();
		CvarWaysAtLatch = Value;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelApplyColumnCacheWays="), Value);
		// 1 and 2 are the only shapes that exist. A clamp rather than a wider
		// ladder because the model above prices 2 ways as worth more than 2x the
		// sets, and nothing has yet measured a third.
		return FMath::Clamp(Value, 1, 2);
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

// THE MID-RUN REFUSAL, and it is the AtlasCoveragePadChunks pattern
// (VoxelWorldSubsystem.cpp:426ff, FVoxelRasterAtlasCpu::WarnCoveragePadChanged)
// stated in this file's terms. The table is sized ONCE, on the first drained
// chunk, and the way count is latched with it; honouring a later change would
// mean re-shaping the table in flight, which drops every live entry and hands
// that tick the whole cold fill -- the exact lump this arm exists to remove, at
// the worst possible moment. So the change is REFUSED and said ONCE.
//
// Warning, not Log: a leg whose cvar no longer matches its table is measuring
// the arm it STARTED with, and a reader has to know that before reading
// anything else on the window line.
//
// Compared against the value the CVAR held at latch, not against the latched
// ways -- a leg that armed with -VoxelApplyColumnCacheWays=2 leaves the cvar at
// 1 quite legitimately, and a warning that fired on every armed leg would stop
// being read, which is worse than not having one.
void WarnWaysChangedIfLate()
{
	static bool bWarned = false;
	if (bWarned || CvarWaysAtLatch < 0)
	{
		return; // not latched yet: nothing has been decided to contradict
	}
	const int32 Live = CVarVoxelStreamShadingCacheWays.GetValueOnGameThread();
	if (Live == CvarWaysAtLatch)
	{
		return;
	}
	bWarned = true;
	UE_LOG(LogVoxelPerf, Warning,
	       TEXT("Voxel apply fast: voxel.Stream.ShadingCacheWays changed to %d after the ")
	       TEXT("shading table was sized at %d way(s) -- REFUSED for this session; the table ")
	       TEXT("is sized once and is not re-shaped in flight. This run is still measuring ")
	       TEXT("ways=%d (slots=%d). Restart with -VoxelApplyColumnCacheWays=%d to change it ")
	       TEXT("-- -ExecCmds lands after streaming has begun and cannot arm this."),
	       Live, CacheWays(), CacheWays(), CacheSlots() * CacheWays(), Live);
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

// THE LRU STAMPS, PARALLEL TO THE TABLE AND ALLOCATED ONLY WHEN ARMED.
//
// NOT a field on FSlot, and the reason is the DEFAULT arm rather than the armed
// one. FSlot is exactly 32 B (three int32, three float, one double -- the "40 B
// per slot" in CacheSlots' comment and in the header's switch table is stale by
// one field and is corrected here); a uint32 added to it becomes 40 B once the
// double's alignment padding is paid, and a 40 B entry no longer divides a 64 B
// line on a table whose every access is a random probe. Kept outside the struct
// the 1-way table is unchanged down to its layout, and an armed SET is
// 2 x 32 B = one cache line's worth.
//
// SIZED IN ONE PLACE, beside the table itself, because two independently sized
// arrays that must agree is the derived-not-verified shape this module keeps
// paying for. Everything that reads Stamps goes through Table() first.
TArray<FSlot> Slots;
TArray<uint32> Stamps;   // EMPTY at 1 way; Slots.Num() entries at 2
uint32 AccessStamp = 0;  // monotonic; wraps, and is compared as a signed diff

TArray<FSlot>& Table()
{
	if (Slots.Num() == 0)
	{
		// SETS x WAYS. CacheSlots() is the SET count -- it was the entry count
		// while the table was direct-mapped and the two were the same number.
		// The entry count is what `slots=` reports on the window line.
		const int32 Ways = CacheWays();
		Slots.SetNum(CacheSlots() * Ways);
		if (Ways > 1)
		{
			Stamps.SetNumZeroed(Slots.Num());
		}
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

// THE ONE TRANSCRIPTION OF THE SLOT KEY, and every site that asks "is this
// footprint cached, and if not, where does its fill go" comes through here:
// ShadingImpl, FillFromPrecomputed, IsCached and the census probe. A second
// spelling of the hash, of the exact key compare or of the way choice is the
// defect shape this module was written to avoid (see StoreSampledSlot), and
// associativity is exactly the kind of change that invites four copies.
//
// PURE: it reads the table and touches nothing. The LRU stamp is refreshed at
// USE -- a served hit, or a fill -- and never at a probe, because IsCached and
// WouldSampleForDispatch are CENSUS, and a census that reordered the eviction
// queue would perturb the arm it exists to measure.
//
// AT 1 WAY THIS IS TODAY'S CODE EXACTLY: Index == the hashed slot, bHit == the
// same three-field compare, bVictimLive == the same Slot.Level >= 0 that
// cacheEvict has always counted, and neither loop below runs a second
// iteration.
struct FLookup
{
	uint32 Index = 0;           // hit: the entry to read. miss: the entry a fill takes.
	int32 Way = 0;              // which way Index is, 0-based (hitWay2= reads this)
	bool bHit = false;          // Index holds this exact (Level, X, Y)
	bool bVictimLive = false;   // MISS ONLY: Index holds a DIFFERENT live entry
	bool bOtherWayLive = false; // MISS ONLY: the way NOT chosen is live
};

FORCEINLINE FLookup LookupSlot(int32 Level, int32 X, int32 Y)
{
	const TArray<FSlot>& T = Table(); // also the lazy sizing, and the ways latch
	const int32 Ways = CacheWays();
	const uint32 Base = SlotIndex(Level, X, Y, uint32(CacheSlots() - 1)) * uint32(Ways);

	FLookup Out;
	for (int32 W = 0; W < Ways; ++W)
	{
		const FSlot& S = T[Base + uint32(W)];
		if (S.Level == Level && S.X == X && S.Y == Y)
		{
			Out.Index = Base + uint32(W);
			Out.Way = W;
			Out.bHit = true;
			return Out;
		}
	}

	// A miss. Take a free way if the set has one -- nothing live is lost and the
	// stamps do not enter it -- and otherwise the older stamp. The compare is a
	// SIGNED DIFFERENCE, not a plain <, so it stays correct across the 32-bit
	// wrap of AccessStamp. A wrong choice at the wrap would still be a legal
	// choice, but a comparison that is only usually right is not one this file
	// will carry.
	int32 VictimWay = 0;
	for (int32 W = 1; W < Ways; ++W)
	{
		if (T[Base + uint32(VictimWay)].Level < 0)
		{
			break;
		}
		const bool bCandFree = T[Base + uint32(W)].Level < 0;
		const bool bCandOlder =
			int32(Stamps[Base + uint32(W)] - Stamps[Base + uint32(VictimWay)]) < 0;
		if (bCandFree || bCandOlder)
		{
			VictimWay = W;
		}
	}
	Out.Way = VictimWay;
	Out.Index = Base + uint32(VictimWay);
	Out.bVictimLive = T[Out.Index].Level >= 0;
	for (int32 W = 0; W < Ways; ++W)
	{
		if (W != VictimWay && T[Base + uint32(W)].Level >= 0)
		{
			// The set is in contention but this miss is not evicting: at 1 way,
			// with the same set index, that live entry WOULD have been thrown
			// away. evictSpared= is that counterfactual, and it is exact for a
			// given key stream -- the stream itself of course changes, because
			// the entries that survive go on to hit.
			Out.bOtherWayLive = true;
		}
	}
	return Out;
}

// The LRU touch. ONE place decides that 1 way writes no stamp, so the default
// arm's hit path stays a pure READ of the entry exactly as it always has been
// and Stamps stays unallocated.
FORCEINLINE void TouchSlot(uint32 SlotIdx)
{
	if (CacheWays() < 2)
	{
		return;
	}
	Stamps[SlotIdx] = ++AccessStamp;
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
// THE TWO ASSOCIATIVITY COUNTERS. cacheEvict above KEEPS ITS MEANING EXACTLY --
// "a live entry was overwritten by a different key" -- at both way counts; the
// arm makes it RARER, it does not redefine it, and renaming or re-scoping it
// would make every 1-way leg ever logged unreadable against an armed one.
//   hitWay2=     demand hits served from the SECOND way. A hard zero at 1 way
//                (there is no second way) and impossible to fake at 2: these
//                are hits that the direct-mapped table could not have had. THE
//                ENGAGEMENT COUNTER -- armed with hitWay2=0 is an inert arm.
//   evictSpared= misses that found a FREE way in a set that still held a live
//                entry. At 1 way, with the same set index, every one of those
//                was an eviction -- so this is the exact count of collision
//                evictions the arm removed from this window's key stream.
//                cacheEvict + evictSpared at 2 ways is the number cacheEvict
//                alone would have read at 1, for the same stream.
int64 HitsWay2 = 0;
int64 EvictsSpared = 0;
int64 SentinelUncached = 0; // "no surface gate" results, never cached (see below)
int64 RootFlushes = 0;
int64 OffThread = 0;        // HARD ZERO: this path is game-thread only
int64 AuditsRun = 0;
int64 AuditMismatches = 0;
double MaxMismatchUU = 0.0;
double SampleSeconds = 0.0; // wall time inside SampleChunkParamsForPool
int64 AuditTick = 0;
// Warm-ahead traffic (voxel.Stream.WarmShadingAhead; see the WARM-AHEAD HOOKS
// block in the header). SEPARATE from Calls/CacheMisses ON PURPOSE: the armed
// leg's verdict is the SUBMIT population's cacheMiss falling, and warm fills
// counted there would relocate every first-touch miss into the same counter
// and erase exactly the reading the A/B is decided on. WarmHits should read
// ~0: the warm loop probes IsCached before calling, and game-thread code has
// no one to race -- a nonzero here is a probe/entry disagreement worth a look,
// not a crash.
int64 WarmFills = 0;
int64 WarmHits = 0;

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

// Forward declaration: both public entry points below are thin wrappers over
// this, and it is DEFINED further down beside the cache it drives. Declared
// here rather than moved, so the cache body stays next to the slot layout and
// the audit it belongs to.
static FVoxelBrickChunkShading ShadingImpl(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                           const FVoxelBrickCpuPackRef& Pack,
                                           const USceneComponent& Root,
                                           FSampleParamsFn SampleParams,
                                           FShadingFromFn ShadingFrom,
                                           bool bAllowGuard,
                                           bool bWarmAhead = false);

// The two halves of ShadingImpl that FillFromPrecomputed (below) also needs,
// FACTORED RATHER THAN COPIED. FillFromPrecomputed writes the same slot from a
// sample taken on a worker, so it must do the same anchor test and the same
// store -- and a second transcription of the anchor rule or the Z rebase is the
// exact defect shape this module was written to avoid (see the CachedRootLoc
// block and FSlot::BaseZUU). One definition, two callers, both in this file.
//
// The slot is addressed by INDEX rather than by reference so that FSlot -- which
// lives in the anonymous namespace above -- stays out of a declaration at this
// scope. Both callers already hold the index from the one SlotIndex() call they
// make, so nothing is recomputed.
static FVector AnchorTableToRoot(const USceneComponent& Root);
static bool StoreSampledSlot(uint32 SlotIdx, const VoxelCoords::FVoxelLevelChunkKey& Key,
                             const FVector4f& Params, double ChunkWorldZ, bool bWarmFill);

FVoxelBrickChunkShading ShadingForPublishSlow(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                              const FVoxelBrickCpuPackRef& Pack,
                                              const USceneComponent& Root,
                                              FSampleParamsFn SampleParams,
                                              FShadingFromFn ShadingFrom)
{
	return ShadingImpl(Key, Pack, Root, SampleParams, ShadingFrom, /*bAllowGuard*/ true);
}

// THE DIRECT CACHE-ENTRY API the dispatch site's own comment asks for.
//
// SubmitGpuMeshJob CONSUMES the shading it builds, so the publish guard must be
// unreachable here -- previously that was arranged by disengaging VoxelApplyFast
// entirely whenever the guard bit was set, which meant the shipping mode (3) ran
// the raw four-column expression at this site while caching it at the other one.
//
// WHY IT MATTERS MORE HERE THAN AT THE DRAIN SITE, measured 2026-08-23 on
// q-a1024.log (per-tick apply cap raised to 1024, so apply stopped being the
// bound and this site became it):
//
//   window   dispatch ms   submit ms   reqHdr ms   calls   perCallUs
//        2         412.4       339.6       299.5   15998        21.2
//        4         850.3       811.1       753.7    8756        92.6
//        6         854.3       825.0       658.3    4496       183.5
//
// reqHdr is footprint/seed/shading/skirt, and the shading is four
// GetSurfaceHeightUU calls that each take the FINE-TIER SAMPLER LOCK
// EXCLUSIVELY and run a full vxc::Amplifier::column on the game thread. As
// throughput rises, CPU worker jobs contend for that same lock and the per-call
// cost goes 21 us -> 183 us. Dispatch then consumed the entire game-thread tick
// (2,456 ms of a 2,000 ms window) and the tick rate collapsed from ~50 Hz to
// ~3 Hz. The apply-fast brief predicted this number ("under sampler-lock
// contention it read 328 us/call") and said to read reqHdr off a leg before
// applying this hook. This is that reading.
//
// FAILING READING: cacheHit ~= 0 at this site with cacheMiss ~= calls means the
// dispatch population has no column reuse -- one entry per chunk, not per
// column -- and this hook is pure loss. Read it on the 'Voxel apply fast' line
// with the drain site's traffic, since both feed the same table.
FVoxelBrickChunkShading ShadingForDispatchSlow(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                               const USceneComponent& Root,
                                               FSampleParamsFn SampleParams,
                                               FShadingFromFn ShadingFrom)
{
	return ShadingImpl(Key, FVoxelBrickCpuPackRef(), Root, SampleParams, ShadingFrom,
	                   /*bAllowGuard*/ false);
}

// --- the warm-ahead hooks (see the header's WARM-AHEAD HOOKS block) ----------

bool IsCached(int32 Level, int32 X, int32 Y)
{
	// GAME THREAD ONLY, like every read of Table(). Off-thread the honest
	// answer is "do not warm this footprint", which reads as cached -- the same
	// fail-toward-doing-nothing direction the sampler's offThread check takes.
	if (!IsInGameThread())
	{
		return true;
	}
	if ((Mode() & kModeCache) == 0 || !bCacheAnchored)
	{
		// No table to hit (cache arm off) or nothing anchored yet. "Not cached"
		// is literally true, but a caller must gate on CacheArmed() before
		// spending a sample on it -- with the cache off the sample cannot be
		// stored and warming is pure loss.
		return false;
	}
	// Through the one lookup, so the probe cannot disagree with the path it is
	// probing for -- at 2 ways "cached" means EITHER way holds the key, and a
	// probe that only looked at way 0 would send the warm loop to re-fill
	// entries that are already there.
	return LookupSlot(Level, X, Y).bHit;
}

// --- the cold-burst census probe (header: THE COLD-BURST CENSUS PROBE) -------
//
// A MIRROR OF ShadingImpl's DECISION CHAIN, DELIBERATELY IN THIS FILE. Every
// early exit below is a copy of a decision made a hundred lines down in
// ShadingImpl, and it is only defensible because it sits beside that function
// over the same statics and calls the same SlotIndex -- there is no second
// transcription of the slot key or the anchor test, which is the defect shape
// this module was written to avoid. If ShadingImpl's chain changes, THIS
// CHANGES WITH IT; the failing reading that would catch a drift is a census in
// which cold shadings stop tracking the 'Voxel apply fast' line's cacheMiss.
//
// Order matches ShadingImpl exactly:
//   Mode() == 0        -> the wrapper folded to the raw expression: always samples
//   off game thread    -> ShadingImpl's offThread arm samples unconditionally
//   cache bit off      -> no table exists: every call samples
//   anchor mismatch    -> the table is flushed and then sampled
//   slot miss          -> samples      (MISS = no WAY of the set holds the key)
//   slot hit           -> WARM (the audit re-sample is the stated inexactness)
//
// The publish guard is not in this list because it cannot fire at the dispatch
// site: ShadingForDispatchSlow passes bAllowGuard = false.
bool WouldSampleForDispatch(const VoxelCoords::FVoxelLevelChunkKey& Key, const USceneComponent& Root)
{
	const int32 M = Mode();
	if (M == 0)
	{
		return true;
	}
	if (!IsInGameThread())
	{
		return true;
	}
	if ((M & kModeCache) == 0)
	{
		return true;
	}
	if (!bCacheAnchored || Root.GetComponentLocation() != CachedRootLoc || Root.GetWorld() != CachedWorld)
	{
		// ShadingImpl clears every slot here and falls through to the sampler,
		// so this call is cold whatever the slot below happens to hold. Rare
		// (counted as rootFlushes on the window line) but not zero, and a
		// census that missed it would under-report a whole tick's burst.
		return true;
	}
	return !LookupSlot(Key.Level, Key.Key.X, Key.Key.Y).bHit;
}

void WarmForDispatch(const VoxelCoords::FVoxelLevelChunkKey& Key,
                     const USceneComponent& Root,
                     FSampleParamsFn SampleParams,
                     FShadingFromFn ShadingFrom)
{
	if (!IsInGameThread())
	{
		// The hard zero, same rule as ShadingImpl's: the table is unsynchronised
		// and the fine-tier prefetch is game-thread-gated. Unlike ShadingImpl
		// there is no consumer to serve, so the right response is to do NOTHING,
		// loudly countable, rather than sample anyway.
		++OffThread;
		return;
	}
	if ((Mode() & kModeCache) == 0)
	{
		// Nothing can be persisted; the warm loop's window line reports
		// INERT-CACHE-OFF for this state so a leg cannot silently burn its
		// budget warming a cache that does not exist.
		return;
	}
	ShadingImpl(Key, FVoxelBrickCpuPackRef(), Root, SampleParams, ShadingFrom,
	            /*bAllowGuard*/ false, /*bWarmAhead*/ true);
}

// (c) THE ASYNC WARM'S LANDING SITE -- voxel.Stream.WarmShadingAsync.
//
// The same slot write WarmForDispatch performs, minus the sampling: the caller
// already holds a FVector4f that a worker produced from the identical math (see
// ChunkShadingParamsFromCorners in VoxelWorldSubsystem.cpp, which both the
// demand sampler and the warm task call). Nothing here samples, so nothing here
// can touch the fine tier, the amplifier, or any UObject beyond the root's own
// transform -- which is the whole reason the async arm is legal at all: the
// TABLE stays game-thread-only, and only the ARITHMETIC moved off-thread.
//
// GAME-THREAD ONLY, same as everything that touches Table(). Off-thread this
// does nothing and counts offThread -- the hard zero.
//
// COUNTS AS warmFill=, NEVER AS cacheMiss, for the reason stated at the
// WarmFills declaration: the armed leg's verdict is the SUBMIT population's
// cacheMiss falling, and a fill counted there would relocate the miss into the
// counter the A/B is decided on and erase the reading.
//
// THE ANCHOR IS RE-TESTED HERE, and it has to be. AnchorTableToRoot compares
// the root's location and world against what the table was filled under and
// flushes on a mismatch -- so a root that moved between the worker's sample and
// this call flushes the table and then stores an entry that was sampled at the
// OLD origin. That is why the caller must ALSO compare the root location it
// captured at launch against the current one and drop the result before calling
// (asyncStale= on the warm-ahead line); this function cannot see the launch-time
// value and does not pretend to.
//
// Returns true iff a slot was written. False means one of: off the game thread,
// the cache arm is off, a live entry already covers this footprint (counted
// warmHit=), or the sample carries the no-surface-gate sentinel (sentinel=).
bool FillFromPrecomputed(const VoxelCoords::FVoxelLevelChunkKey& Key,
                         const USceneComponent& Root,
                         const FVector4f& Params)
{
	if (!IsInGameThread())
	{
		++OffThread;
		return false;
	}
	if ((Mode() & kModeCache) == 0)
	{
		// Nothing can be persisted. Same response as WarmForDispatch: do
		// nothing, and let the warm loop's INERT-CACHE-OFF suffix say so.
		return false;
	}

	const FVector OriginRelative = VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level);
	const FVector RootLoc = AnchorTableToRoot(Root);
	const double ChunkWorldZ = RootLoc.Z + OriginRelative.Z;
	const FLookup Look = LookupSlot(Key.Level, Key.Key.X, Key.Key.Y);

	if (Look.bHit)
	{
		// A demand sample (or an earlier fill) beat the task home. Keep what is
		// there: the two are the same function of the same inputs, but the one
		// already in the table was taken on this thread against residency the
		// game thread had just checked. Same policy DrainAssetResolveResults
		// takes for a raced resolve, and the same warmHit= counter
		// WarmForDispatch uses for the identical state.
		++WarmHits;
		return false;
	}

	// Look.Index is the free way when the set has one and the older of the two
	// when it does not -- a warm fill evicts under the same LRU rule a demand
	// fill does. Still uncounted in cacheEvict, for the reason the miss path
	// below states: warm traffic must not land in the counter the A/B is
	// decided on.
	return StoreSampledSlot(Look.Index, Key, Params, ChunkWorldZ, /*bWarmFill*/ true);
}

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

	WarnWaysChangedIfLate();

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
	       TEXT("Voxel apply fast (window): mode=%d(%s%s%s) calls=%lld avoided=%lld (%.1f%%) ")
	       TEXT("| guardSkip=%lld (withPack=%lld) cacheHit=%lld cacheMiss=%lld cacheEvict=%lld ")
	       TEXT("hitWay2=%lld evictSpared=%lld ")
	       TEXT("sentinel=%lld | sampled=%lld sampleUs/sample=%.2f sampleUs/call=%.2f ")
	       TEXT("sampleMsWindow=%.1f | audit=%lld mismatch=%lld maxMismatchUU=%.4f ")
	       TEXT("| warmFill=%lld warmHit=%lld | rootFlush=%lld offThread=%lld ")
	       TEXT("ways=%d sets=%d slots=%d win=%.1fs"),
	       M,
	       (M & kModeGuard) ? TEXT("guard") : TEXT("-"),
	       (M & kModeCache) ? TEXT("+cache") : TEXT(""),
	       (M & kModeMeasure) ? TEXT("+measure") : TEXT(""),
	       (long long)Calls, (long long)Avoided,
	       Calls > 0 ? 100.0 * double(Avoided) / double(Calls) : 0.0,
	       (long long)GuardSkipped, (long long)GuardSkipWithPack,
	       (long long)CacheHits, (long long)CacheMisses, (long long)CacheEvicts,
	       (long long)HitsWay2, (long long)EvictsSpared,
	       (long long)SentinelUncached,
	       (long long)Sampled, SampleUsPerSample, SampleUsPerCall, SampleSeconds * 1000.0,
	       (long long)AuditsRun, (long long)AuditMismatches, MaxMismatchUU,
	       (long long)WarmFills, (long long)WarmHits,
	       (long long)RootFlushes, (long long)OffThread,
	       // slots= IS THE ENTRY COUNT, not the set count, so an armed leg reads
	       // 262144 where a control leg reads 131072 and cannot be mistaken for
	       // one. ways= says the same thing without arithmetic; both print
	       // because an arm that can be read as inert is the house failure.
	       CacheWays(), CacheSlots(), CacheSlots() * CacheWays(), WindowSec);

	Calls = Sampled = GuardSkipped = GuardSkipWithPack = 0;
	CacheHits = CacheMisses = CacheEvicts = SentinelUncached = 0;
	HitsWay2 = EvictsSpared = 0;
	WarmFills = WarmHits = 0;
	RootFlushes = OffThread = 0;
	AuditsRun = AuditMismatches = 0;
	MaxMismatchUU = 0.0;
	SampleSeconds = 0.0;
	LastLogSeconds = Now;
}

// --- the two factored halves (declared beside ShadingImpl's forward decl) ----

// Anchor check (see CachedRootLoc). Exact compare, no tolerance: a moved root
// means every cached corner height was sampled at the wrong XY, and "nearly the
// same place" is not a defensible basis for keeping them. Returns the root's
// CURRENT component location, which is also the Z base every caller rebases
// against, so the value the table is anchored to and the value the caller
// computes with cannot drift apart.
//
// GAME THREAD ONLY (it can flush the whole table); both callers check first.
static FVector AnchorTableToRoot(const USceneComponent& Root)
{
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
	return RootLoc;
}

// The slot write, including the sentinel refusal. Returns true iff the slot was
// actually written. bWarmFill counts the write into warmFill= -- the counter
// warm traffic uses so it can never be confused with a demand cacheMiss.
static bool StoreSampledSlot(uint32 SlotIdx, const VoxelCoords::FVoxelLevelChunkKey& Key,
                             const FVector4f& Params, double ChunkWorldZ, bool bWarmFill)
{
	if (Params.Z <= kSurfaceSentinelCeiling)
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
		return false;
	}
	FSlot& Slot = Table()[SlotIdx];
	Slot.Level = Key.Level;
	Slot.X = Key.Key.X;
	Slot.Y = Key.Key.Y;
	Slot.Temperature = Params.X;
	Slot.Precipitation = Params.Y;
	Slot.GradPacked = Params.W;
	Slot.BaseZUU = double(Params.Z) + ChunkWorldZ;
	TouchSlot(SlotIdx); // a fill is a use; no-op at 1 way
	if (bWarmFill)
	{
		++WarmFills; // the slot write IS the product of a warm call
	}
	return true;
}

// The shared body. bAllowGuard is the ONLY difference between the two public
// entry points: at the drain site the pack decides whether anything consumes
// the result, and at the dispatch site the result is always consumed, so the
// guard must not be reachable there at all. Passing that as a parameter keeps
// ONE implementation of the cache -- a second transcription of the slot key or
// the Z rebase is the defect shape this module was written to avoid.
static FVoxelBrickChunkShading ShadingImpl(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                           const FVoxelBrickCpuPackRef& Pack,
                                           const USceneComponent& Root,
                                           FSampleParamsFn SampleParams,
                                           FShadingFromFn ShadingFrom,
                                           bool bAllowGuard,
                                           bool bWarmAhead)
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

	// Warm-ahead traffic stays OUT of Calls (and out of the flush cadence,
	// which keys off it) -- see the WarmFills declaration for why.
	if (!bWarmAhead)
	{
		++Calls;
		// The window flush costs one clock read; amortised over 256 drained
		// chunks it is free, and at 12,700 drains per 5 s window the log still
		// lands within ~0.1 s of its nominal edge. Pass -- to FlushStats from
		// MaybeLogCounters (optional hook 3) if exact alignment with
		// `Voxel tick budget` is wanted.
		if ((Calls & 255) == 0)
		{
			FlushStats(/*bForce*/ false);
		}
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
	if (bAllowGuard && (M & kModeGuard) && !WillPublish(Pack))
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

	const FVector RootLoc = AnchorTableToRoot(Root);

	const double ChunkWorldZ = RootLoc.Z + OriginRelative.Z;
	// AFTER AnchorTableToRoot, always: that call can flush every slot, and a
	// lookup taken before it would name an entry that no longer exists.
	const FLookup Look = LookupSlot(Key.Level, Key.Key.X, Key.Key.Y);
	const uint32 SlotIdx = Look.Index;

	if (Look.bHit)
	{
		const FSlot& Slot = Table()[SlotIdx];
		// The LRU refresh, and it is here rather than in LookupSlot because a
		// PROBE must not reorder the eviction queue (see LookupSlot). No-op at
		// 1 way. Warm hits refresh too: the warm loop asking for a footprint is
		// evidence the anchor is walking toward it, which is exactly what the
		// stamp is for -- and unlike the counters, the stamp is not a
		// population the A/B is read on.
		TouchSlot(SlotIdx);
		if (bWarmAhead)
		{
			// The warm loop probes IsCached before calling, so this is ~0 by
			// construction -- counted rather than assumed, because silent
			// success is the house failure. No audit either: audits exist to
			// check what is PUBLISHED, and nothing consumes a warm result.
			++WarmHits;
			return FVoxelBrickChunkShading::Neutral();
		}
		++CacheHits;
		if (Look.Way > 0)
		{
			// A hit the direct-mapped table could not have had. Counted on the
			// DEMAND population only, so it divides into cacheHit= directly.
			++HitsWay2;
		}

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

	if (!bWarmAhead)
	{
		++CacheMisses;
		if (Look.bVictimLive)
		{
			// A live entry for a DIFFERENT column is being overwritten. THE
			// MEANING IS UNCHANGED AT BOTH WAY COUNTS -- at 2 ways it means both
			// ways were live and neither was this key, so the older one goes.
			// cacheEvict ~= cacheMiss with a low hit rate is the table thrashing
			// and wants -VoxelApplyColumnCache raised; cacheEvict ~= 0 with a low
			// hit rate means the KEY is wrong, which is a completely different
			// problem.
			++CacheEvicts;
		}
		else if (Look.bOtherWayLive)
		{
			// 2 ways only, and unreachable at 1 by construction: the set was in
			// contention and the fill took the free way instead of evicting.
			++EvictsSpared;
		}
	}

	// A warm sample bypasses the Sample() lambda so its wall time lands in the
	// warm loop's own warmBudgetMs (the `Voxel warm-ahead` window line) and not
	// in SampleSeconds -- Sampled would not move with it and sampleUs/sample
	// would read inflated. A warm fill CAN overwrite a live entry for another
	// column and is still not counted in cacheEvict: the displaced column
	// re-misses through the counted path, so cacheEvict/cacheMiss still names
	// it, and a warm write landing in the counter the A/B is decided on is the
	// exact contamination WarmFills exists to prevent. (The "rare enough not to
	// earn a counter" that used to be written here was WRONG, and the arithmetic
	// at CacheWays() above says why: at a 0.56 load factor the collision rate is
	// 43%, not a rounding error. It is uncounted by policy, not by rarity.)
	const FVector4f Fresh = bWarmAhead ? SampleParams(Root, OriginRelative, Key.Level)
	                                   : Sample();

	StoreSampledSlot(SlotIdx, Key, Fresh, ChunkWorldZ, /*bWarmFill*/ bWarmAhead);

	return ShadingFrom(Fresh);
}

} // namespace VoxelApplyFast
