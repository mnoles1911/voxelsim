// VoxelResidencyGpu.h -- T4-2 "GPU-resident residency": the manager for the
// compute-pass desired-set walks (VoxelResidencyScan.usf).
//
// docs/gpu-residency-t42-plan.md is the design note; the shader's own header
// comment is the data-structure contract. This file is the game-thread seam.
//
// ===========================================================================
// WHAT THIS IS, IN ONE PARAGRAPH
// ===========================================================================
// RecomputeDesiredSet's eviction walk is O(tracked records) and its entry
// scans are O(annulus cells); at 271,549 records they are the streaming
// pipeline's limiter (admission ~65% + exit walk ~30% of recompute,
// Saved/phase1-split.log). Both walks are pure geometry over the anchor plus
// two mirrorable inputs -- the tracked-key set and each footprint's Z range
// -- so both run here as compute over a GPU shadow of that state, appending a
// compact admit/resurrect/evict delta that the CPU adjudicates in O(delta)
// against the state only the game thread may touch: fine-tile residency
// (which CANNOT be queried off the game thread), edits, parked geometry,
// admission caps. Aokana's transferable lesson (arXiv 2505.02017) is
// DECOUPLE, DON'T RELOCATE: nothing here is ever waited on. Deltas ride
// FRHIGPUBufferReadback and are consumed whenever they arrive.
//
// ===========================================================================
// MODES (command-line latched: -VoxelGpuResidency=N)
// ===========================================================================
//   0  OFF (default). Every entry point returns immediately; the streaming
//      control arm is byte-identical. Command line rather than a cvar for
//      -VoxelPendingJobCap's reason: an -ExecCmds cvar lands after streaming
//      has already built its desired set, so an -ExecCmds A/B silently
//      measures the same state twice.
//   1  SHADOW. The pass runs and its deltas are read back and COMPARED
//      against the decisions the CPU walks actually made for the same
//      recompute, but nothing is consumed -- streaming behaviour is unchanged
//      (the notes and the dispatch add cost, deliberately accepted in this
//      arm; the perf arm is mode 2). This is the validation arm and it is
//      what tonight's leg runs.
//   2  LIVE (wired 2026-08-23, per the design note's §7). The CPU walks are
//      SKIPPED on the steady path; the subsystem consumes the newest arrived
//      delta (TakeLiveDelta) and adjudicates it in O(delta): admit/resurrect
//      proposals through the extracted AdmitCandidateEvaluate (fine gate,
//      parks, caps, overlay routing all intact), evict proposals through the
//      SAME ExitVisitRecord lambda the CPU walk runs -- which re-derives the
//      three exit tests in double against THIS call's anchor, so a stale or
//      wrong proposal is vetoed, never obeyed. The CPU remains the sole
//      adjudicator of residency; the GPU only proposes.
//
//      The fallback lanes that stay CPU, each counted on the live log line
//      (the owner's standing decision -- "retain edits and cold fallback on
//      CPU"):
//        * underground recomputes (deep box; no dispatch, skipUG),
//        * a level's true FIRST scan (cold start; liveCpuFirstScans),
//        * cold Z-texel footprints (CPU-enumerated, budgeted per call),
//        * edited footprints in the annulus (enumerated every consume --
//          the texel mirror never carries the edit hatches),
//        * mirror-orphaned records (the residual ledger, walked per call),
//        * resync replays (audit drift), and
//        * the STARVATION fallback: no delta for kLiveStarvationCalls
//          consecutive consume-eligible recomputes runs the full CPU walks
//          that call (liveCpuFallback). A dead GPU path therefore degrades
//          to the CPU arm and READS as liveCpuFallback climbing -- it cannot
//          impersonate a working GPU path.
//
// ===========================================================================
// THE COMPARISON GATE, AND WHAT FAILURE LOOKS LIKE
// ===========================================================================
// Never gate on a statistic that cannot come out the other way. The [gpu-resid]
// line's failing readings, stated in advance:
//
//   admitMissedSurface > 0   the GPU failed to propose an above-ground,
//                            warm-texel, non-boundary admission the CPU made.
//                            A kernel that appends NOTHING maximises this
//                            number -- a dead pass cannot read as a pass.
//   evictMissed/evictExtra > 0 (non-boundary)  the exit kernel and the CPU
//                            walk disagree about a tracked record.
//   audit mismatch (repeated) the mirror has drifted from ChunkRecords and
//                            resync did not heal it.
//   proposals all zero over a moving-flight window  while the CPU admitted
//                            thousands -- shows up as admitMissedSurface, and
//                            ALSO directly as admit=0 on the traffic line.
//   cold not shrinking       the Z-range texel lane never warms; the pass is
//                            handing everything back to the CPU.
//
// Boundary-classified mismatches (float-vs-double within epsilon of a radius
// or the cutoff) are counted separately and are EXPECTED to be small and
// stable; they are not failures.
//
// ===========================================================================
// THREADING
// ===========================================================================
// Every public method is GAME THREAD ONLY, matching the streaming-state rule
// at the top of FVoxelWorldImpl. Internally the manager enqueues render
// commands and polls readbacks from a game-thread ticker; nothing here is
// called from workers.

#pragma once

#include "CoreMinimal.h"

// One ring level's captured constants. Filled by the RecomputeDesiredSet seam
// from the same accessors the CPU walks read, ONCE PER DISPATCH, by value --
// the cvar-capture rule, applied to everything (the manager and the shader
// must never read a knob themselves; a knob read in two places is a knob that
// can disagree with itself).
struct FVoxelResidencyLevelParams
{
	double ChunkEdgeUU = 0.0;
	double OuterUU = 0.0;        // ring outer radius
	double AdmitOuterUU = 0.0;   // outer + max(half-diagonal, overlap band)
	double InnerAdmitUU = 0.0;   // padded inner admission edge
	double InnerEvictUU = 0.0;   // inner eviction edge (hysteresis gap inward)
	double UnloadOuterUU = 0.0;  // outer * UnloadRingMultiplier
	double VerticalKeepUU = 0.0; // deep-record vertical hysteresis
	double CutoffSortKeySq = 0.0; // this ring's admission cutoff (post-relaxation)
	FIntVector AnchorChunk = FIntVector::ZeroValue; // anchor's chunk at this level
	int32 ChunkSpan = 0;         // annulus box half-span in chunks
	bool bScanThisDispatch = false; // entry-scan gate verdict for this level
};

struct FVoxelResidencyDispatchParams
{
	FVector Anchor = FVector::ZeroVector;
	int32 NumLevels = 0;         // VoxelCoords::kNumLevels at the call site
	int32 MaxRingLevel = 0;
	bool bHierarchicalCoverage = false;
	int32 RingOverlapChunks = 0;
	bool bAnchorUnderground = false; // true => no dispatch (CPU-only recompute), counted
	bool bUndergroundDisabled = false;
	double SkirtNearFrac = 0.0;
	double SkirtMidFrac = 0.0;
	int32 SkirtChunksNear = 0;
	int32 SkirtChunksMid = 0;
	int32 SkirtChunksFar = 0;
	FVoxelResidencyLevelParams Levels[8];
};

// One GPU proposal, unpacked for the game-thread adjudicator. For admit /
// resurrect / evict lanes Coord is the level-L chunk coordinate; for the cold
// lane Coord.Z is meaningless (a cold entry names a FOOTPRINT).
struct FVoxelResidencyProposal
{
	int32 Level = 0;
	FIntVector Coord = FIntVector::ZeroValue;
};

// Mode 2: one arrived scan's proposals, handed whole to the subsystem.
// Params is the dispatch's own capture -- its Anchor / per-level AnchorChunk /
// bScanThisDispatch are what the consumer stamps the per-level scan state
// from (the GPU scan of level L at anchor A is, for the gating machinery,
// "level L was scanned at A").
struct FVoxelResidencyLiveDelta
{
	uint32 Seq = 0;
	uint32 OverflowBits = 0; // 1=admit 2=evict 4=resurrect 8=cold 16=orphan (truncated prefix, rest re-proposes)
	FVoxelResidencyDispatchParams Params;
	TArray<FVoxelResidencyProposal> Admits;
	TArray<FVoxelResidencyProposal> Evicts;
	TArray<FVoxelResidencyProposal> Resurrects;
	TArray<FVoxelResidencyProposal> Colds; // footprints with no valid Z texel: the CPU-enumeration lane
};

// Mode 2: what the subsystem's adjudication of one recompute actually did,
// reported back so every live lane -- including every FALLBACK lane -- shows
// on the [gpu-resid] live line. The failing readings are documented at the
// log site; the rule is the file's usual one: no lane may be a statistic that
// cannot come out the other way.
struct FVoxelResidencyLiveOutcome
{
	uint32 ConsumedDelta = 0;      // 1 if a delta was consumed this recompute
	uint32 NoDeltaCalls = 0;       // consume-eligible recompute found no delta ready
	uint32 CpuFallbackCalls = 0;   // starvation: full CPU walks ran this call
	uint32 CpuFirstScans = 0;      // levels swept CPU-side because never scanned (cold start)
	uint32 AdmitProposals = 0;
	uint32 AdmitAdmitted = 0;      // records actually created (direct + nearest-admit commit)
	uint32 AdmitAdopted = 0;       // proposals answered from parked geometry
	uint32 AdmitResurrected = 0;   // proposals (either lane) that cancelled a pending unload
	uint32 AdmitStale = 0;         // already tracked, not pending -- mirror/readback lag
	uint32 AdmitRejFine = 0;       // fine-tile gate refused (re-proposed by construction)
	uint32 AdmitRejBudget = 0;     // per-level budget refused this call
	uint32 AdmitRejCutoff = 0;     // ring cutoff refused (CPU-side re-check of the kernel's gate)
	uint32 ResurrectProposals = 0;
	uint32 EvictProposals = 0;
	uint32 EvictQueued = 0;        // proposals the double recheck CONFIRMED (unload queued)
	uint32 EvictVetoed = 0;        // recheck at this call's anchor said keep (stale/boundary)
	uint32 EvictStale = 0;         // no record (already removed) or already pending
	uint32 ColdProposals = 0;
	uint32 ColdEnumerated = 0;     // cold footprints CPU-enumerated this call
	uint32 ColdDeferred = 0;       // over the per-call cold budget; re-proposed next scan
	uint32 EditedEnumerated = 0;   // edited footprints in the annulus enumerated this consume
	uint32 ResidualWalked = 0;     // orphaned records walked with the CPU evict math
	float EvictMs = 0.f;           // the live exit stage's cost (replaces the record walk)
	float AdmitMs = 0.f;           // the live entry stage's cost (replaces the cell sweeps)
};

class VOXELEARTHSHADERS_API FVoxelResidencyGpu
{
public:
	static FVoxelResidencyGpu& Get();

	// Latched from -VoxelGpuResidency= on first call; 0 unless passed. Every
	// other entry point self-gates on this, so call sites need no guard --
	// but the RecomputeDesiredSet seam checks it once anyway to skip building
	// the params struct in mode 0.
	int32 GetMode();

	// True once per enable and after any detected drift: the caller must
	// replay the full record state (NoteRecordAdded for every ChunkRecord,
	// NoteUnloadQueued for every pending-unload key) before OnRecomputeBegin.
	// The replay rides the same feedback lane as everything else, so order
	// and dedup come for free.
	bool NeedsResync();
	void BeginResyncReplay();  // clears staged feedback and resets CPU-side ledgers
	void EndResyncReplay();    // marks the mirror synced (upload rides next dispatch)

	// --- The recompute bracket ----------------------------------------------
	// Begin: flushes staged feedback + Z-range texels and dispatches the scan
	// chain for this recompute (unless underground / in-flight ring full --
	// both counted, both visible on the log line). Opens the decision ledger
	// for this sequence number so the notes below are recorded as THIS
	// recompute's CPU decisions.
	void OnRecomputeBegin(const FVoxelResidencyDispatchParams& Params);
	// End: closes the ledger. Also where ready readbacks are consumed and
	// compared (plus a ticker, so a hovering anchor still drains them).
	void OnRecomputeEnd();

	// --- Mutation notes ------------------------------------------------------
	// One call per ChunkRecords / PendingUnloadSet transition, at the exact
	// site of the transition. These are BOTH the mirror's feedback stream and
	// (inside a bracket) the CPU decision ledger the comparison holds the GPU
	// against. Mode 0: immediate return.
	void NoteRecordAdded(int32 Level, const FIntVector& Coord, bool bDeepAnchorRelative);
	void NoteRecordRemoved(int32 Level, const FIntVector& Coord);
	// Eviction decision + pending mark. bDeepAnchorRelative rides along so the
	// comparator can re-run the vertical keep-test in double for this key.
	void NoteUnloadQueued(int32 Level, const FIntVector& Coord, bool bDeepAnchorRelative);
	void NoteUnloadCancelled(int32 Level, const FIntVector& Coord); // resurrection decision + pending clear
	// The Z-range texel mirror. Called from FootprintChunkZRangeCached on hit
	// and on fill with the MEMO'S values (pre-skirt ZMin, trimmed ZMax); the
	// manager dedups against its CPU-side mirror map and stages only changes.
	void NoteFootprintZRange(int32 Level, int32 X, int32 Y, int32 ChunkZMin, int32 ChunkZMax);

	// --- Mode 2 (LIVE) ------------------------------------------------------
	// True when a consumable delta is staged. TickStreaming uses this to force
	// a recompute while the anchor is stationary (consumption happens ONLY
	// inside RecomputeDesiredSet, where the queue sort/filter that must follow
	// any admission or eviction already runs). Cheap: a latched-int check plus
	// a pointer test; modes 0/1 always return false.
	bool HasActionableDelta();
	// Move the newest staged delta out for adjudication. Polls first, never
	// waits: false means "propose nothing this call", not "stall". An older
	// staged delta that was superseded before consumption is counted, never
	// silently merged.
	bool TakeLiveDelta(FVoxelResidencyLiveDelta& Out);
	// The orphan ledger (records aliased out of the mirror): the live arm must
	// walk these with the CPU evict math every call, because the GPU exit scan
	// can no longer see them. O(residual); empty in the overwhelming case.
	void SnapshotResidual(TArray<FVoxelResidencyProposal>& Out);
	// The subsystem reports what its adjudication of this recompute did; the
	// manager folds it into the [gpu-resid] live line's window.
	void NoteLiveOutcome(const FVoxelResidencyLiveOutcome& Outcome);

	// PIE teardown / world switch: drop buffers, ledgers, stats; next enable
	// resyncs. Safe to call in any mode.
	void Reset();

private:
	FVoxelResidencyGpu() = default;
	struct FImpl;
	FImpl* Impl = nullptr;
	FImpl& GetImpl();
};
