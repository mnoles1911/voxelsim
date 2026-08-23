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
//   2  LIVE -- NOT YET WIRED. Requested mode 2 clamps to 1 with a warning.
//      The adjudication path (consume deltas, skip the CPU walks) needs
//      AddCandidate extracted from RecomputeDesiredSet's entry loop into a
//      callable member, which is deliberately deferred until the three
//      concurrent VoxelWorldSubsystem.cpp edit waves land. The design note
//      carries the full wiring plan.
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

	// PIE teardown / world switch: drop buffers, ledgers, stats; next enable
	// resyncs. Safe to call in any mode.
	void Reset();

private:
	FVoxelResidencyGpu() = default;
	struct FImpl;
	FImpl* Impl = nullptr;
	FImpl& GetImpl();
};
