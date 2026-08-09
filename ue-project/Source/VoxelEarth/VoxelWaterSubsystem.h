#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelCoords.h" // VoxelCoords::FVoxelCoord -- UE-only, voxel-core-free (see VoxelWorldSubsystem.h's identical doctrine note)
#include "VoxelDebug.h"  // FVoxelWaterPerfSnapshot -- plain POD, voxel-core-free
#include "VoxelWaterSubsystem.generated.h"

// W2 first engine slice (docs/voxel-earth-implementation-plan.md SS3.7 Layer
// B "THE AUTHORITY"; voxel-core/include/voxelcore/waterca.h is the tested,
// bit-deterministic CA reference this subsystem drives live). PImpl, exactly
// like UVoxelWorldSubsystem (VoxelWorldSubsystem.h's doctrine note applies
// verbatim here): this UHT-parsed header must never include a voxel-core
// header, so FVoxelWaterImpl (defined only in VoxelWaterSubsystem.cpp) owns
// the actual vxc::WaterCA plus every piece of water-specific bookkeeping
// (reservoir cells, dirty-chunk tracking, replication accumulators).
//
// Authority-only simulation (task spec item 1): server/listen/standalone
// tick the CA on a fixed 10Hz accumulator (Tick()); NM_Client never steps
// its own CA, only mirrors state received via AVoxelEditRelay::
// MulticastWaterDiffs (ApplyReplicatedWaterDiffs) -- see the .cpp for the
// role-split detail, which mirrors UVoxelWorldSubsystem's own NM_* branching
// doctrine (docs/m3-plan.md "Authority flow").
//
// Takes a direct dependency on UVoxelWorldSubsystem (Collection.
// InitializeDependency, both are UWorldSubsystem-derived, so ordering is
// handled by the subsystem collection, not by any fragile BeginPlay race):
// the CA's SolidFn bridges to UVoxelWorldSubsystem::IsSolidAtVoxel (already
// public, overlay-aware) rather than duplicating terrain access.
struct FVoxelWaterImpl;

// --- W4 SWE diagnostics (read-only) -----------------------------------------
//
// Three PODs and three const queries, added for -VoxelSweBreachTest
// (AVoxelEarthGameMode) and for nothing else. They exist because the question
// that fixture answers -- "is the sheet SPREADING correctly, or are the beds
// SEATED WRONG?" -- cannot be answered from the outside: the SweGrid lives
// inside FVoxelWaterImpl, which this header must never see, and the 1Hz
// SwePerf line reports whole-sheet totals only. A surge is a per-column
// VELOCITY signature at a known place at a known time, and a mis-seated bed is
// a per-column disagreement between a stored int and the terrain; neither is
// visible in a total.
//
// Plain structs (not USTRUCT) and plain scalar members, so the doctrine above
// holds: no voxel-core type crosses this header. All three queries are const,
// allocate nothing, mutate no simulation state, and are safe to call at any
// time from the game thread; none of them is on any hot path.

// Whole-sheet state, i.e. the SwePerf status line in struct form plus the
// grid's placement (which the log line does not carry and a fixture needs in
// order to know whether the column it cares about is even inside the sheet).
struct FVoxelSweProbe
{
	bool bArmed = false;      // a SweGrid + SweCaCoupler exist right now
	int64 SheetVolume = 0;    // vxc::SweGrid::totalVolume(), fill units (255 == 1 voxel)
	int64 CaVolume = 0;       // vxc::WaterCA::totalVolume(), same units
	int64 Injected = 0;       // the running "total injected" the ADR-0004 invariant compares against
	int64 ConservationFailures = 0; // ticks on which ca+sheet != injected
	int32 SweColumns = 0;     // columns currently SWE-owned
	int32 LastPunctured = 0;  // columns the last coupler step found punctured (bed no longer solid)
	int64 TransferredToCA = 0;   // coupler ledger, cumulative fill units SWE -> CA
	int64 TransferredToSWE = 0;  // coupler ledger, cumulative fill units CA -> SWE
	// Beds re-seated after a terrain edit took the ground out from under a sheet
	// column, and how many such columns are still waiting for swe.h S5(a) to
	// finish metering them out. Both are diagnostics for the breach fixture:
	// a carve inside the sheet that moves NEITHER of these is a carve the sheet
	// has not noticed, which is the failure mode that produced punctured=0.
	int64 BedsReseated = 0;
	int32 PendingBedReseats = 0;
	int64 OriginVx = 0, OriginVy = 0; // sheet's inclusive voxel-column origin
	int32 SizeX = 0, SizeY = 0;       // sheet extent in columns
	int32 SheetScanVoxels = 0;        // SweCoupleConfig::sheetScanVoxels
	int32 SettleToleranceFill = 0;    // sweSettleTolerance(cfg): the derived deadband a settled surface is flat to
};

// Everything about one voxel column that either solver could be holding, plus
// the terrain's own answer for the same column. The three groups are
// deliberately returned together: the whole diagnostic is a COMPARISON between
// them, and sampling them through three separate calls would let them drift
// across a fixed step.
struct FVoxelWaterColumnProbe
{
	int64 Vx = 0, Vy = 0;

	// --- CA side (always filled in) ---
	int64 CaTopVz = 0;      // highest voxel in the scan window with fill > 0
	uint8 CaTopFill = 0;    // that voxel's fill (0 == the column holds no CA water at all)
	int32 CaColumnFill = 0; // sum of CA fill over the scan window, fill units

	// --- terrain side (always filled in) ---
	bool bTerrainTopFound = false; // a solid voxel was found in the scan window
	int64 TerrainTopSolidVz = 0;   // topmost UVoxelWorldSubsystem::IsSolidAtVoxel == true voxel in it

	// --- SWE side (meaningful only when bSweArmed && bInSheet) ---
	bool bSweArmed = false;
	bool bInSheet = false;  // the column is inside the sheet rectangle
	bool bSweOwned = false; // ... and the coupler has promoted it
	int32 Bed = 0;          // vxc::SweGrid::bedAt, the seated bed voxel z
	int32 Depth = 0;        // vxc::SweGrid::depthAt, fill units in the column
	int32 VelXMmPerSec = 0, VelYMmPerSec = 0; // vxc::SweGrid::velocityAt -- THE SURGE SIGNAL
	int32 FluxXFillPerTick = 0, FluxYFillPerTick = 0; // the two faces this column owns
};

// The bed-seating audit. Beds are seated ONCE, when the sheet is armed
// (VoxelWaterSubsystem.cpp's MaybeArmSwe), from the topmost solid voxel in a
// window around GetSurfaceHeightUU, and are never re-seated afterwards. So
// every later disagreement between a stored bed and the ground under it is a
// real defect in the ownership boundary, and the sheet would be resting at the
// wrong height with no other symptom than "the water settles somewhere odd".
//
// The audit re-runs the SAME seating function against the SAME wrapped
// solidity callback, now, and diffs. It deliberately does NOT re-implement the
// seating rule -- a forked copy would agree with the original for exactly as
// long as nobody edited either.
struct FVoxelSweBedAudit
{
	bool bArmed = false;
	int32 Columns = 0;   // columns examined (the whole sheet)
	int32 Seated = 0;    // columns whose stored bed voxel reads SOLID right now
	int32 SweOwned = 0;  // columns currently SWE-owned

	// Stored bed vs. what SeatSweBedZ would return TODAY, through the same
	// mobilizer-wrapped solidity callback the coupler itself uses. Nonzero
	// means the world changed under a seated bed (terrain streamed/edited
	// after arming) and nothing re-seated it.
	int32 Mismatched = 0;
	int32 SweOwnedMismatched = 0; // ... restricted to promoted columns, which is where it MATTERS
	int32 MaxAbsDeltaVoxels = 0;
	int64 SumAbsDeltaVoxels = 0;

	// Stored bed vs. the topmost solid voxel per UVoxelWorldSubsystem::
	// IsSolidAtVoxel -- the BARE terrain query, with no mobilizer wrapper.
	// This one is EXPECTED to differ over an unmobilized implicit cavern lake
	// (the wrapper reads that as solid on purpose, see SeatSweBedZ's comment),
	// so it is reported separately rather than folded into Mismatched: a
	// nonzero count here with Mismatched == 0 is benign, the other way round
	// is not.
	int32 MismatchedVsTerrain = 0;

	// Worst single disagreement (wrapped comparison), for a log line that
	// names a column rather than just counting.
	int64 WorstVx = 0, WorstVy = 0;
	int32 WorstStoredBed = 0, WorstReseatedBed = 0;
};

UCLASS()
class VOXELEARTH_API UVoxelWaterSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UVoxelWaterSubsystem();
	// See UVoxelWorldSubsystem's identical destructor/FVTableHelper doc
	// comment: TUniquePtr<FVoxelWaterImpl>'s destructor needs FVoxelWaterImpl's
	// full definition, which this UHT-parsed header must not see.
	virtual ~UVoxelWaterSubsystem() override;
	UVoxelWaterSubsystem(FVTableHelper& Helper);

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End UWorldSubsystem Interface

	//~ Begin FTickableGameObject / UTickableWorldSubsystem Interface
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject / UTickableWorldSubsystem Interface

	// --- Activation sources v0 (task item 2) ---------------------------------

	// Dev tool (voxel.SpawnWater console command; also the -VoxelSpawnWaterTest
	// command-line switch, AVoxelEarthGameMode): adds Amount fill units,
	// stacking upward, starting at WorldLocationUU's voxel column (see
	// vxc::WaterCA::addWater). Authority-only: refused (logged, returns 0) on
	// NM_Client, mirroring the terrain subsystem's role-split doctrine (a
	// client has no authoritative CA to add to). Returns the amount actually
	// placed.
	uint32 SpawnWaterAt(const FVector& WorldLocationUU, uint32 Amount);

	// Dig-breach hook: called by UVoxelWorldSubsystem right after an
	// AUTHORITATIVE TryDig/CarveSphere turns solid voxels to air (never on a
	// client's local prediction -- same authority-only rule as the CA tick
	// itself).
	//
	// PURELY DIAGNOSTIC SINCE watershed plan §6.4 (work item 8). It used to run
	// the Reservoir v0 seeding rule -- pin every below-datum cleared voxel that
	// touched below-datum air at 255 fill units, forever. That rule could not
	// tell the sea from a pit dug into a hillside, its pressure head was the
	// breach rather than the datum, and because the ocean beyond the breach was
	// not water to the simulation at all it poured unbounded volume (and an
	// unbounded active-brick count) into a seabed the CA thought was dry.
	// voxel-core's tests/test_ocean.cpp measures all three.
	//
	// The sea is now the third term of the water ImplicitFn, so a dig into it
	// releases water through the same funnel a dug lake shore uses --
	// NotifyTerrainRegionEdited -> WaterMobilizer::mobilizeEditRegion, which
	// every caller of this function already calls beside it. What remains here
	// is the log line, kept because it is the only signal an unattended
	// headless run has that a dig opened the sea, and now reporting the DATUM
	// TEST: cleared voxels genuinely in the sea versus cleared voxels merely
	// below z=0 inside land.
	void NotifyTerrainVoxelsCleared(const TArray<VoxelCoords::FVoxelCoord>& ClearedVoxels);

	// ADR-0003 item 2 (docs/adr/0003-hydrostatic-persistent-body.md): the
	// general "terrain solidity changed here" hook the cross-tick solid_ memo
	// (voxelcore/waterca.h setSolidCacheEnabled) needs, distinct from
	// NotifyTerrainVoxelsCleared above -- that one is solid->air ONLY
	// (reservoir/breach seeding never cares about the other direction), while
	// the memo is a cache of solidity ITSELF and must be told about BOTH
	// solid->air AND air->solid (a placed block matters exactly as much as a
	// dug one). Called by UVoxelWorldSubsystem after EVERY authoritative edit
	// (dig/place/carve/island-or-collapse-removal) with that edit's own
	// inclusive voxel-coordinate bounding box -- ONE call per edit, not one
	// per voxel (batched, per ADR-0003's "keep it efficient" note), routed
	// straight to vxc::WaterCA::invalidateSolidRegion. Over-invalidating the
	// box is always safe (a dropped-but-still-valid memo entry costs one
	// re-query, never a wrong answer), so callers may pass a conservative box
	// rather than the exact edited-cell set. No-op on NM_Client and whenever
	// the memo is disabled (nothing to invalidate either way).
	//
	// THIS ALSO WAKES THE WATER. Invalidating the memo only corrects what the
	// CA BELIEVES about terrain; it does not make the CA look. Per
	// voxelcore/waterca.h "Activity / settling", a settled body of water has
	// left the active set entirely and step() over it is a no-op, so before
	// vxc::WaterCA::wakeRegion existed a settled pond sat frozen through any
	// dig/place/carve/collapse underneath or beside it (verified empirically:
	// docs/status.md, "Water reactivation on terrain edits"). This hook is
	// already called from EVERY authoritative edit path, so routing
	// wakeRegion through it here is what makes water react to terrain edits
	// at all. Waking writes no fill -- it is scheduling only, so
	// GetWaterVolume() cannot move because of it.
	void NotifyTerrainRegionEdited(const VoxelCoords::FVoxelCoord& MinVoxelIncl, const VoxelCoords::FVoxelCoord& MaxVoxelIncl);

	// Diagnostic: whether the cross-tick solid_ memo (voxel.Water.SolidCacheEnabled)
	// is currently enabled -- read by verification/perf-report log lines so a
	// dumped digest or timing line is unambiguous about which path produced it.
	bool IsSolidCacheEnabled() const;

	// docs/debug-tooling-plan.md P3 "Water" HUD row (task item 3): a snapshot
	// refreshed at 1Hz (same cadence/shape convention as
	// UVoxelWorldSubsystem::GetPerfSnapshot), read by AVoxelEarthHUD every
	// frame when voxel.Debug >= 1.
	FVoxelWaterPerfSnapshot GetPerfSnapshot() const;

	// Deterministic digest over the whole water body (vxc::WaterCA::digest)
	// -- the W2 determinism-verification primitive (task item 5d), mirroring
	// UVoxelWorldSubsystem::GetEditedDigest.
	uint64 GetWaterDigest() const;

	// Diagnostic/verification helper: true (with OutCenterUU set to the
	// centroid of every currently-stored water brick, in world UU) if any
	// water is stored at all; false (OutCenterUU untouched) otherwise. Used
	// by -VoxelSpawnWaterTest/-VoxelBreachTest's screenshot framing
	// (AVoxelEarthGameMode) to aim the verification camera at wherever the
	// CA's lateral spread actually settled, rather than assuming it stayed
	// exactly at the pour/breach column.
	bool GetStoredWaterCentroidUU(FVector& OutCenterUU) const;

	// Diagnostic: the single highest per-cell fill value (0-255) across
	// every currently-stored water brick -- 0 if nothing is stored. Used to
	// distinguish "genuinely no water left" from "water is stored but every
	// cell is below the >=128 v0 meshing threshold" (task item 4's stub
	// rule for partial-fill surface cells) when a verification run's
	// screenshot shows nothing visible.
	uint8 GetMaxStoredFill() const;

	// --- W4 SWE diagnostics (see the three PODs above) -----------------------

	// Whole-sheet state. False (Out untouched apart from bArmed) when the sheet
	// is not armed, so a caller can use the return value as "is W4 live".
	bool GetSweProbe(FVoxelSweProbe& Out) const;

	// One column, from all three sides at one instant. ScanFromVz is the top of
	// the scan window and ScanVoxels its downward extent, in voxels; they bound
	// the CA-fill and terrain-topmost searches (the SWE half is O(1) and ignores
	// them). Returns false only when the subsystem has no Impl.
	bool GetWaterColumnProbe(int64 Vx, int64 Vy, int64 ScanFromVz, int32 ScanVoxels,
	                         FVoxelWaterColumnProbe& Out) const;

	// Re-seats every sheet column's bed with the live world and diffs it against
	// the stored bed. Walks the whole sheet (128x128 columns, each a downward
	// solidity scan), so this is a one-shot verification call, NOT something to
	// put on a tick. False when the sheet is not armed.
	bool AuditSweBedSeating(FVoxelSweBedAudit& Out) const;

	// --- C7/C8 underground water (docs/cavern-design.md SS5) -----------------

	// Searches outward from OriginUU for a cavern column that is BOTH carved
	// (a room actually reaches it) and flooded, and returns a point on its
	// water surface in world UU. Steps the search grid at 30 m, which is
	// under a cavern site's ~36 m reach radius, so no site inside the radius
	// can be stepped over. Cost is one worldgen column query per grid point;
	// this is verification/debug scaffolding (-VoxelFloodTest), not a
	// gameplay path. Returns false if nothing flooded is in range.
	bool FindFloodedCavernNear(const FVector& OriginUU, double SearchRadiusUU, FVector& OutWaterSurfaceUU) const;

	// The column's implicit flood level in world UU, or false if the column is
	// dry. Pure worldgen (vxc::cavernFloodedAt / CavernColumn::floodZMm) --
	// zero storage, no simulation state consulted.
	bool GetCavernFloodZUU(double WorldXUU, double WorldYUU, double& OutFloodZUU) const;

	// Carves an outflow tunnel from a flooded cavern horizontally outward until
	// it leaves the site's flood reach, then slopes it down -- i.e. gives the
	// lake somewhere to actually go. Needed because the flood field is defined
	// on CURRENT air below floodZ, so digging DOWNWARD inside a flooded column
	// only creates more implicit water (correct aquifer semantics, and exactly
	// what makes a cavern lake feel like groundwater); draining requires
	// breaking OUT of the flooded columns. Returns voxels removed.
	// Verification scaffolding for -VoxelFloodTest; authority only.
	int32 CarveCavernOutflow(const FVector& LakeSurfaceUU);

	// The implicit field's CURRENT contribution at a world point (0-255), i.e.
	// respecting the mobilization handover: 0 once the owning brick has
	// converted to CA water. Verification/debug read.
	uint8 GetImplicitFillAtWorld(const FVector& WorldUU) const;

	// --- "Am I in water?", for real ------------------------------------------
	//
	// Every underwater/swim test in the client used to be `Z < 0`: below the
	// sea-level datum, full stop, with no terrain and no water consulted. That
	// makes a DRY CAVERN 200 m under a mountain read as ocean -- global
	// underwater fog, a tinted post-process, and a swimming character, inside
	// solid rock's air pocket. It also makes every visual judgement of every
	// later water milestone unreadable, which is why
	// docs/watershed-system-plan.md puts fixing it first (§5.3, item 1).
	//
	// Two queries, deliberately separate:

	// WATER THAT IS ACTUALLY THERE: the CA's fill, plus the implicit field for
	// bricks it has not taken over yet (implicitFillAt already returns 0 once
	// a brick is mobilized, so the two never double-count). Today that means
	// cavern lakes and anything a player poured or breached; when the baked
	// lake/river datum joins the implicit field (plan items 3-4 and 7) it
	// starts answering for those too with NO change here -- which is the point
	// of routing the test through the water datum rather than the camera.
	// Returns 0-255 fill units, 0 for dry.
	uint8 GetWaterFillAtWorld(const FVector& WorldUU) const;

	// THE FULL PREDICATE the client should use: simulated/implicit water, OR
	// the open sea. See IsOpenSeaAtWorld for what "the open sea" is allowed to
	// mean before the ocean is a real datum (plan item 8).
	bool IsUnderwaterAtWorld(const FVector& WorldUU) const;

	// Sea level in UE units, from voxelcore/core.h's kSeaLevelMm -- the one
	// symbol, converted once, so nothing in the client spells the datum as a
	// literal again.
	static double SeaLevelZUU();

	// THE OCEAN, AS A DATUM. True when a point is in the open sea: below sea
	// level, and standing over WORLDGEN ground that is itself below sea level
	// (a seabed) rather than over land.
	//
	// The second condition is the whole fix. "Below sea level" alone is what
	// tinted dry caverns; "below sea level over a seabed" is false inside a
	// mountain, false in a dry pit dug into land (the worldgen ground there is
	// still above the datum, which is why this takes the worldgen surface and
	// NOT the edited one), and true where the sea actually is.
	//
	// What it still gets wrong, stated rather than hidden: a natural inland
	// depression whose baked floor lies below sea level reads as sea. Nothing
	// short of a real water datum can tell that apart from a bay, and supplying
	// one is exactly what plan items 3-4 do. Until then this is strictly less
	// wrong than the camera test, and the failure is rarer and diagnosable.
	static bool IsOpenSeaAtWorld(double WorldZUU, double WorldgenGroundZUU);

	// C8 ledger, for verification logging. Shortfall MUST be 0 forever: it
	// counts units the implicit field gave up that the CA did not accept,
	// which is the one way mobilization could destroy water. See waterca.h.
	void GetMobilizationStats(int32& OutMobilizedBricks, uint64& OutDebited, uint64& OutCredited,
	                          uint64& OutShortfall) const;

	// --- Replication plumbing v1 (task item 1) -------------------------------
	//
	// Server -> client transport is AVoxelEditRelay::MulticastWaterDiffs
	// (VoxelEditRelay.h/.cpp), broadcast by this subsystem's Tick() at a
	// fixed ~5Hz cadence, byte-capped per broadcast (see the .cpp's
	// BroadcastWaterDiffs/SerializeWaterDiffs for the wire format and cap --
	// brick-granularity raw-fill snapshots, NOT a per-cell delta; full
	// compression is explicitly W2-polish per the task spec). This method is
	// the client-side receive half, called by AVoxelEditRelay::
	// MulticastWaterDiffs_Implementation on every remote (non-authority)
	// instance: applies the batch to this client's local mirror CA (via
	// vxc::WaterCA::setReplicatedFill, NOT addWater -- a replicated snapshot
	// sets cells to their authoritative values directly) and dirties every
	// affected chunk for re-mesh. Returns false on a corrupt/short buffer.
	bool ApplyReplicatedWaterDiffs(const TArray<uint8>& Bytes);

	// --- ADR-0005 water persistence (docs/adr/0005-water-persistence.md) ------
	//
	// Serializes the WaterState blob (vxc::WaterState::serialize -- per-brick CA
	// fill + the active set + the mobilized brick keys) and writes it as a
	// SIBLING of the terrain edit log's own save. UVoxelWorldSubsystem::SaveWorld
	// persists Saved/VoxelWorlds/<seed>.vxlog; this writes Saved/VoxelWorlds/
	// <seed>.vxwater right beside it -- same directory, same per-seed naming, the
	// same atomic tmp+rename write, and the same lifetime (autosaved from
	// Deinitialize on shutdown, loaded from OnWorldBeginPlay). It is deliberately
	// a separate file, not a section of the log: water invalidates on
	// kWaterCAVersion and is discardable without discarding terrain edits (see
	// the .cpp and waterca.h's WaterState comment).
	//
	// Authority only (server/listen/standalone) -- a client has no authoritative
	// CA to persist; returns false (logged) on NM_Client, on a missing Impl, or
	// on a write failure. Also invoked directly by the voxel.SaveWater console
	// command and the -VoxelWaterPersistTest fixture.
	bool SaveWaterState() const;

	// Diagnostic (task item: prove the drain->save->reload round trip in-engine):
	// serializes the LIVE CA+mobilizer to the exact bytes SaveWaterState writes,
	// then reads the on-disk .vxwater file back and applies it (vxc::WaterState::
	// load, fills-first) into a FRESH CA+mobilizer pair, and reports both sides'
	// digest / total volume / mobilized-brick count. Byte-identical digests plus
	// conserved volume prove a save/load cycle keeps every drained cavern rather
	// than trusting that the serializer's own unit tests mean the UE file wiring
	// is correct. False if no blob could be read or parsed. Non-const: it walks
	// the amplifier column memo to rebuild the fresh pair's implicit field.
	bool VerifyWaterDiskRoundTrip(uint64& OutLiveDigest, uint64& OutReloadedDigest, uint64& OutLiveVolume,
	                              uint64& OutReloadedVolume, int32& OutLiveMobilized, int32& OutReloadedMobilized);

	// --- LAKE SHEETS AT RANGE (docs/watershed-system-plan.md item 5, §5.2) ----
	//
	// The near field meshes implicit water inside a 52 m brick disc and nothing
	// outside it, so a 2 km lake is ABSENT from every vista. AVoxelWaterSheetActor
	// draws the rest as flat translucent rectangles at the datum; these three
	// methods are the only thing it needs from the water tier, and they are here
	// rather than on the actor because this subsystem already owns the ONE
	// fine-tier lake reader and a second one would be a second world.
	//
	// All three are no-ops (return 0/false) when there is no fine tier -- the
	// same supported "no baked lakes" configuration MakeWaterSampler logs.

	// The bbox and datum of one baked basin, in world UU. A copy, deliberately:
	// the voxel-core registry it comes from is owned by a tile that can be
	// evicted, and an actor holding a pointer across a rebuild would dangle.
	struct FLakeSheetBasin
	{
		int32 TileX = 0;
		int32 TileY = 0;
		int32 BasinId = 0;
		double MinXUU = 0.0, MinYUU = 0.0, MaxXUU = 0.0, MaxYUU = 0.0;
		double SurfaceZUU = 0.0;
	};

	// Which fine tile a world point falls in. Exposed so a caller can walk the
	// tiles of a region ONE PER TICK rather than asking for the whole region at
	// once -- a fine tile is tens of MB on disk, and a 10 km square is up to 9
	// of them, which is a multi-second game-thread stall if taken in one gather.
	static void FineTileForWorldUU(double XUU, double YUU, int32& OutTileX, int32& OutTileY);

	// Every water-holding basin of ONE fine tile whose bbox meets the square of
	// half-extent RadiusUU around (CenterXUU, CenterYUU). Loads that tile, so it
	// is game-thread only and does disk I/O; the caller budgets it. Returns the
	// number appended.
	int32 GatherLakeSheetBasinsInTile(int32 TileX, int32 TileY, double CenterXUU, double CenterYUU,
	                                  double RadiusUU, TArray<FLakeSheetBasin>& Out) const;

	// One basin's wet extent as world-UU rectangles at the given decimation
	// (`StepPx` fine pixels per emitted cell, >= 1). Appends; returns the count.
	// False-y (0 rectangles) also means "this basin's tile would not resolve",
	// which is why OutUnresolved is separate from an empty result: a lake that
	// failed to decode must not read as a lake with no water in it.
	int32 BuildLakeSheetRects(const FLakeSheetBasin& Basin, int32 StepPx, TArray<FBox2D>& OutRectsUU,
	                          bool& bOutResolved) const;

	// The XY footprint the implicit-water refresh is CURRENTLY meshing, plus the
	// z span of its brick disc, so the sheet can cut an exact hole where the near
	// field already draws water. False before the first refresh has run.
	// The rectangle is inclusive of the whole 65-brick disc, i.e. exactly what
	// RefreshImplicitWater sweeps -- not an approximation of it.
	bool GetImplicitWaterDiscUU(FBox2D& OutXY, double& OutMinZUU, double& OutMaxZUU) const;

	// --- THE BASIN VOLUME LEDGER (water re-architecture Phase 2) --------------
	//
	// The authoritative water state for standing water is a signed int64 volume
	// per basin, against the baked equilibrium the bake shipped. These two are
	// the whole of what a caller outside this file needs: move the scalar, and
	// read what it did. Everything else -- the capacity curve, the spillway, the
	// save blob, the replication rows -- follows from the scalar and lives in
	// the .cpp beside voxel-core.
	//
	// PLAIN INTEGER TYPES ONLY, because this header is UHT-parsed and must never
	// see a voxel-core type (the same rule FVoxelWaterImpl exists to keep). A
	// basin is named the way the v1 wire names it: its tile plus its tile-local
	// row index.

	// Adds `Units` (one unit == one WaterCA fill unit == 1/255 of a 10 cm voxel)
	// to a basin, spilling whatever will not fit below its baked sill.
	//
	// Returns the amount ACCEPTED -- `Units` when the basin resolves, 0 when it
	// does not (no fine tier, tile not streamed, no such row). A caller keeping
	// its own ledger must use the return value and never the request. `OutLevelMm`
	// and `OutBakedMm` report where the lake now stands and where the bake said
	// it stands, so the difference is legible without a second query.
	int64 CreditBasinVolume(int32 TileX, int32 TileY, int32 LocalBasinId, int64 Units,
	                        int32& OutLevelMm, int32& OutBakedMm);

	// Ledger session stats. `OutBasins` is how many are off equilibrium;
	// `OutSpilledUnits` is what has left basins over their sills; `OutRouted` and
	// `OutRefunded` split that into what the routing graph took and what came
	// back. bLedgerActive false means there is no fine tier and none of the rest
	// means anything -- which is a different fact from every counter reading 0.
	void GetBasinLedgerStats(bool& bOutLedgerActive, int32& OutBasins, int64& OutSumUnits,
	                         int64& OutSpilledUnits, int64& OutRoutedUnits, int64& OutRefundedUnits) const;

	// --- FAR-FIELD RIVER RIBBONS (docs/water-handover-2026-08-04.md Phase 4) --
	//
	// The lake half above draws basins. Rivers cannot use it: `basinsForTile`,
	// `holdsWater()` and `extentMaskFor` all assume a basin, and
	// CompositeWaterSampler forwards the sheet half to lakes on purpose --
	// "a river reach is not a flat disc and cannot be drawn as one". So flowing
	// water had NO far-field path at all and was invisible past the 52 m
	// implicit disc. voxelcore/riverribbon.h is the producer; these four
	// methods are the only thing AVoxelRiverRibbonActor needs from the water
	// tier, and they live here for the same reason the lake sheet trio does:
	// this subsystem owns the ONE fine-tier water reader.
	//
	// THE STAGES ARE SEPARATE BECAUSE THE COST IS. riverribbon.h says it
	// outright ("the host does not call buildRiverRibbons -- it needs the
	// stages separately so it can budget the fill across ticks"): the wet-mask
	// fill decodes 256x256 water blocks off disk and is the expensive half,
	// while thin/trace/simplify are near-linear scans of a mask that is ~0.01%
	// wet. So the fill is banded across ticks and the rest runs once.
	//
	// All four are no-ops when there is no fine tier, the same supported
	// "no baked water" configuration MakeWaterSampler logs.

	// One centreline vertex, already in world UU. `HalfWidthUU` is HALF the
	// width the BAKE ACTUALLY DREW at this point, not channelWidthMm's
	// discharge law -- the ribbon has to cover the same raster the near-field
	// voxels were meshed from or the two will not line up at the handover.
	struct FRiverRibbonVertexUU
	{
		double XUU = 0.0, YUU = 0.0, ZUU = 0.0;
		double HalfWidthUU = 0.0;
	};

	// One reach, ordered along the channel. Direction is not meaningful.
	struct FRiverRibbonPathUU
	{
		TArray<FRiverRibbonVertexUU> Points;
	};

	// Opens a wet-mask window of half-extent RadiusUU around (CenterXUU,
	// CenterYUU) and returns how many FillRiverRibbonWindowBand calls it will
	// take to fill. Allocates one byte per fine pixel -- at the default 4 km
	// radius that is a 4267 px square, 18 MB -- so a caller that opens a window
	// must finish or abandon it rather than opening a second.
	// Returns 0 when there is no fine tier or the radius is degenerate.
	int32 BeginRiverRibbonWindow(double CenterXUU, double CenterYUU, double RadiusUU);

	// Fills band `BandIndex` of the open window from the baked water plane.
	// Game-thread only: it decodes water blocks, which is disk I/O plus zstd.
	// Returns false if there is no open window or the index is out of range.
	bool FillRiverRibbonWindowBand(int32 BandIndex, int64& OutWetPixels);

	// Thins the filled mask to a centreline, resolves the datum at the
	// centreline pixels ONLY (~0.3% of wet, and the only place the 16-probe
	// spline is actually wanted), traces ordered paths and simplifies them
	// against a one-pixel perpendicular tolerance -- the step that removes the
	// 8-connected staircase without touching a meander. Appends to OutPaths,
	// returns the number appended, and CLOSES the window (frees the mask).
	int32 FinishRiverRibbonWindow(TArray<FRiverRibbonPathUU>& OutPaths, int64& OutWetPixels,
	                              int64& OutCentrePixels, int64& OutUnresolvedBlocks);

	// Frees an open window without building anything. Safe to call when none is
	// open; that is the point -- a re-gather mid-fill must not leak 18 MB.
	void AbandonRiverRibbonWindow();

private:
	TUniquePtr<FVoxelWaterImpl> Impl;

	// M3-wave-2-style autosave gate, mirroring UVoxelWorldSubsystem's own
	// bWorldBegunPlay: set true once OnWorldBeginPlay runs its genuine
	// game-world/Impl-present body, so Deinitialize's water autosave never writes
	// the empty state of the transient "Entry"/loading world's phantom subsystem
	// instance over a real save file.
	bool bWorldBegunPlay = false;

	// Single actor hosting every UWaterChunkComponent (mirrors
	// UVoxelWorldSubsystem's ChunkOwner/ChunkRoot pattern exactly, but kept
	// entirely separate from terrain's -- this subsystem owns its own actor).
	// Left null on NM_DedicatedServer (no viewport; see OnWorldBeginPlay) --
	// the CA still ticks authoritatively regardless, only rendering is
	// skipped, same reasoning as UVoxelWorldSubsystem's dedicated-server path.
	UPROPERTY(Transient)
	TObjectPtr<AActor> ChunkOwner;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> ChunkRoot;

	// M_WaterVoxel (Tools/create_water_voxel_material.py), falling back to
	// the engine default material if missing -- same never-crash doctrine as
	// UVoxelWorldSubsystem's ChunkMaterial resolution.
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> WaterMaterial;
};
