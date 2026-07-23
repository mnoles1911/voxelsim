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
	// client's local prediction -- breach seeding is simulation state, same
	// authority-only rule as the CA tick itself). For every cleared voxel at
	// or below sea level (Z<0) that borders a cell OUTSIDE this same edit
	// which is already non-solid, Z<0, and not itself tracked CA water (i.e.
	// genuinely pre-existing "implicit ocean", not a neighbor this same dig
	// just carved), registers a Reservoir v0 boundary cell: seeded to full
	// fill now and topped back up to full every fixed step thereafter for as
	// long as it's registered (task item 2's "infinite reservoir" contract).
	// Documented v0 simplification: a reservoir cell, once registered, is
	// never un-registered (no support yet for "plugging" a breach back up);
	// see the .cpp for the full rule.
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
