#include "VoxelWaterSubsystem.h"

#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelEditRelay.h"
#include "VoxelWaterChunkComponent.h"
#include "VoxelFineTileStreamer.h"  // CoarseTileForWorldMm -- one addressing rule
#include "VoxelTileCodec.h"         // GetFineTileDecompressor -- the CODEC_ZSTD boundary
#include "VoxelWorldSubsystem.h"

// ADR-0006 water pool (voxel.Water.GPU): a SECOND INSTANCE of the terrain
// geometry pool, not a copy of it. See GetOrCreateWaterPool below for what had
// to be parameterised and what did not.
#include "VoxelGpuPoolComponent.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (same doctrine note as VoxelWorldSubsystem.cpp).
#include "voxelcore/amplifier.h"
// Water re-architecture Phase 2: the scalar hydrology authority. The ledger,
// the client-reconstructed capacity curve and the two fine-tier adapters.
#include "voxelcore/basinledger.h"
#include "voxelcore/bytes.h"
#include "voxelcore/caverns.h"
#include "voxelcore/lakes.h"
#include "voxelcore/mesher.h"
// W4 shallow water (docs/adr/0004-swe-fixed-point-coupling.md). This include is
// the first one anywhere in the repo: swe.h shipped complete, tested and
// GOLDEN-PINNED but entirely unreferenced -- "nothing in voxel-core or the
// engine constructs a SweGrid" -- and this file is where that stops being true.
// Nothing in voxel-core changed to make that possible; the header was always
// callable, it was only ever waiting on ADR-0004 item 3.
#include "voxelcore/swe.h"
// W3 river network + its coupling to the CA (plan S3.7 Layer R). Same story
// swe.h had one milestone ago: rivernet.h/rivercouple.h shipped complete,
// tested and golden-pinned but unreferenced by the engine, and this file is
// where that stops being true. Nothing in voxel-core changed to allow it.
#include "voxelcore/rivercouple.h"
#include "voxelcore/rivernet.h"
// Phase 4, the far-field river producer. Same story swe.h and rivernet.h each
// had one milestone ago: riverribbon.h shipped complete, tested and verified
// against RiverSampler at 0 disagreements in 5,480,281 cells, and entirely
// unreferenced by the engine -- which is exactly why rivers were invisible
// past the 52 m implicit disc. This file is where that stops being true.
#include "voxelcore/riverribbon.h"
#include "voxelcore/tiles.h"
#include "voxelcore/waterca.h"
#include "voxelcore/waterwindow.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator (locating the world's single AVoxelEditRelay)
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"  // ADR-0005 water persistence: IFileManager (atomic rename, mkdir)
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h" // ADR-0005 water persistence: FFileHelper (blob read/write)
#include "Misc/Paths.h"      // ADR-0005 water persistence: FPaths::ProjectSavedDir (mirror the .vxlog path)
#include "TimerManager.h"    // voxel.Water.SpawnIn's deferred pour
#include "UObject/UObjectGlobals.h"

#include <memory>
#include <algorithm>
#include <vector>

// ADR-0003 item 2/4 (docs/adr/0003-hydrostatic-persistent-body.md): toggles
// vxc::WaterCA's cross-tick terrain-solidity memo. Checked every fixed step
// (see StepFixed below), same "read the cvar every call, no restart needed"
// pattern as VoxelWorldSubsystem.cpp's CVarVoxelDestructionEnabled/
// CVarVoxelCollapseEnabled -- so it can be flipped off in the field without a
// relaunch (task item 4) if the invalidation contract is ever suspected of a
// gap. Byte-identical for as long as UVoxelWorldSubsystem's edit paths honour
// the invalidation contract (NotifyTerrainRegionEdited, wired into
// TryDig/TryPlace/CarveSphere/island-and-collapse-removal -- see
// VoxelWorldSubsystem.cpp).
//
// DEFAULT ON as of ADR-0003 item 2 resolution: an in-engine cross-process A/B
// (-VoxelWaterMemoTest, VoxelEarthGameMode.cpp) ran the full dig/place/carve/
// M5-collapse edit vocabulary beneath and around a settled basin pool, once
// with this cvar forced 0 and once forced 1 (same seed, same edit sequence),
// and diffed EVERY logged checkpoint digest, not just the final one: pour,
// pre-edit settle, post-dig/-place/-carve/-collapse (each immediately after
// its edit AND again once re-settled). All of them matched byte-for-byte
// between the two runs, including intermediate values, confirming the
// invalidation wiring is complete for every edit path this cvar's contract
// depends on. See docs/status.md's "Water edit-notification completeness +
// memo enablement" entry for the full digest table and the perf numbers.
static TAutoConsoleVariable<bool> CVarVoxelWaterSolidCache(
	TEXT("voxel.Water.SolidCacheEnabled"), true,
	TEXT("ADR-0003: enable WaterCA's cross-tick terrain-solidity memo (~2.8-3.0x hydrostatic pass speedup). ")
	TEXT("Byte-identical for as long as every terrain edit invalidates it -- see docs/adr/0003-hydrostatic-persistent-body.md. ")
	TEXT("Set to 0 to force the uncached path (e.g. if an edit path is ever suspected of a missed invalidation)."),
	ECVF_Default);

// THE OCEAN TERM (docs/watershed-system-plan.md work item 8, §6.4).
//
// DEFAULT ON, because it is the feature; the cvar exists to be the CONTROL.
// Every visual judgement this repo has got wrong this month was a frame with
// no control beside it, and "the same shot with the term off" is the only way
// to tell an ocean artefact from a pre-existing one. Turning it off restores
// the pre-item-8 implicit field EXACTLY (voxel-core's
// `ocean_term_off_is_exactly_the_pre_item_8_field` asserts that half) -- but
// NOT Reservoir v0, which this change retires and which off-ness does not
// bring back. See tests/test_ocean.cpp for what it did and what it cost.
//
// READ ON THE GAME THREAD ONCE PER MOBILIZATION SWEEP, not inside the
// ImplicitFn: the field must be a pure, deterministic function of position for
// as long as any brick's mobilization state depends on it, and a cvar flipped
// mid-tick would make the wall invariant a function of wall-clock time. It is
// therefore latched into FVoxelWaterImpl::bImplicitOcean at construction and
// re-read only in Tick, before the fixed-step loop -- the same discipline
// MaybeArmSwe uses for voxel.Water.SWE and for the same reason.
static TAutoConsoleVariable<bool> CVarVoxelWaterImplicitOcean(
	TEXT("voxel.Water.ImplicitOcean"), true,
	TEXT("Watershed §6.4: the sea as the third term of the water ImplicitFn -- open air below ")
	TEXT("kSeaLevelMm in columns whose WORLDGEN ground is below it. Gives the ocean the same wall, ")
	TEXT("budgeted mobilization, ledger, replication and persistence lakes already have, and makes a ")
	TEXT("dug inland pit stay dry by testing the datum instead of the camera. 0 = the control: no ")
	TEXT("implicit ocean at all (NOT the retired Reservoir v0, which does not come back)."),
	ECVF_Default);

// --- Watershed §6.5, work items 9b/9c: the mobilized ceiling and the return
// --- path (docs/water-handover-2026-08-04.md Phase 2) ------------------------
//
// READ THE MEASUREMENT BEFORE RETUNING EITHER OF THESE. voxel-core's
// `waterca_no_ceiling_can_fill_a_breach_over_the_hydrostatic_cap` pins the fact
// that decides both defaults: `kMaxHydrostaticComponentCells` (waterca.cpp:108)
// leaves an over-cap connected body COMPLETELY unlevelled, and the excavation's
// own cells are CA territory that no mobilization policy has any say over. So
// the ceiling is a COST bound and nothing else. It cannot make a large breach
// fill correctly, and the value at which a breach does fill correctly (~100
// bricks, measured) is about 170x SMALLER than a legitimate world's mobilized
// set already is -- one savegame on this branch restored 17,235 mobilized
// bricks. The two jobs want values three orders of magnitude apart, so this
// cvar does exactly one of them.
static TAutoConsoleVariable<int32> CVarVoxelWaterMobilizedCeiling(
	TEXT("voxel.Water.MobilizedCeilingBricks"), 65536,
	TEXT("Watershed §6.5.5 (work item 9b): hard bound on how many bricks the activity-driven ")
	TEXT("mobilization FRONT may convert. A MEMORY/COST backstop, not a correctness knob -- it bounds ")
	TEXT("the runaway a breach into the open sea causes (measured: activeBricks 19,636 -> 41,613, ")
	TEXT("volume 501M -> 884M) but it does NOT make an over-cap breach level correctly; see ")
	TEXT("kMaxHydrostaticComponentCells. Default 65,536 bricks ~= 32 MB of fill: far above any ")
	TEXT("legitimate world (a savegame here restored 17,235) so it only ever catches a true runaway. ")
	TEXT("Edits and replication are EXEMPT and may exceed it. 0 = no ceiling (the pre-9b behaviour)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelWaterDemoteBudget(
	TEXT("voxel.Water.DemoteBudgetBricks"), 32,
	TEXT("Watershed §6.5.3 (work item 9c): how many mobilized bricks the authority EXAMINES per fixed ")
	TEXT("step, demoting those that already hold exactly the datum. This is the return path -- without ")
	TEXT("it `mobilized_` is insert-only and a persistent world leaks bricks, savegame and replication ")
	TEXT("forever. The budget counts examinations, not demotions, because the 512-cell scan is the ")
	TEXT("cost. Authority only. 0 = disabled (the pre-9c behaviour)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelWaterCeilingReliefBudget(
	TEXT("voxel.Water.CeilingReliefBudgetBricks"), 256,
	TEXT("Watershed §6.5.5: the bigger demotion sweep spent at the high-water mark, at most once per ")
	TEXT("advanceFront and only while at the ceiling, so a world under its ceiling never pays for it. ")
	TEXT("'Reclaim, then refuse' -- whatever this frees is visible to the rest of that same call. ")
	TEXT("0 = refuse without reclaiming first (the ceiling still holds, it just holds by refusing)."),
	ECVF_Default);

namespace
{
// -VoxelWaterMarkerOnly=1: draw ONLY the debug marker's magenta voxels, with the
// near-field implicit disc, the lake sheets and the river ribbons all silent.
//
// Resolved ONCE from the command line, like every other switch that decides what
// gets built rather than how it is shaded (see GpuMeshEnabled's note on why
// -ExecCmds is too late for those). It is read every tick, hence the static.
bool VoxelWaterMarkerOnly()
{
	static const bool bOnly = []
	{
		int32 Flag = 0;
		return FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterMarkerOnly="), Flag)
			       ? Flag != 0
			       : FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterMarkerOnly"));
	}();
	return bOnly;
}

// BrickKey <-> VoxelCoords::FVoxelCoord: this subsystem carries brick-grid
// coordinates (NOT voxel coordinates) through UE-visible plumbing as
// FVoxelCoord, per that struct's W2 doc comment (VoxelCoords.h) -- these two
// tiny helpers are the one place the reinterpretation happens.
VoxelCoords::FVoxelCoord ToCoord(const vxc::BrickKey& K)
{
	return VoxelCoords::FVoxelCoord{K.x, K.y, K.z};
}
vxc::BrickKey ToBrickKey(const VoxelCoords::FVoxelCoord& C)
{
	return vxc::BrickKey{static_cast<int32_t>(C.X), static_cast<int32_t>(C.Y), static_cast<int32_t>(C.Z)};
}

// --- Replication wire format (task item 1) ----------------------------------
//
// Brick-granularity raw-fill snapshots (u32 count, then per brick: i32 x,y,z
// + 512 raw fill bytes in the SAME fixed cell order WaterBrick8::digest uses)
// -- NOT a per-cell delta. Deliberately simple (task spec: "basic diff
// encoding -- full compression is W2-polish"); built from vxc::ByteWriter/
// ByteReader, the same primitives UVoxelWorldSubsystem's edit-log wire format
// uses (VoxelWorldSubsystem.cpp's SerializeEntries/ParseEntries).
//
// Processes Keys in order, stopping once MaxBytes would be exceeded (but
// always encoding at least one brick, guaranteeing forward progress even if
// a single brick's 524 bytes alone exceeds a pathologically small cap).
// OutConsumedKeyCount reports how many of the FRONT of Keys were fully
// handled (encoded OR determined-absent-so-nothing-to-send) -- the caller
// drops exactly those from its "dirty since last broadcast" set, leaving any
// remainder (past the byte cap) for the next ~5Hz round.
//
// --- THE KEY-REMOVAL SECTION (work item 9c) ---------------------------------
//
// A trailing `u32 removalCount` then `i32 x,y,z` per demoted brick. waterca.h
// requires demotion to replicate as an EXPLICIT key removal and never to be
// inferred client-side, which is why it needs wire bytes of its own: every
// other state change here is carried implicitly by the arrival of fill, and a
// demotion is precisely the change whose signature is the ABSENCE of fill.
//
// IT GOES LAST, AND THE ORDER IS THE CONTRACT. The authority drains
// takeRecentlyMobilized() before takeRecentlyDemoted() (waterca.h), so a brick
// that mobilized and demoted inside one broadcast window must land on
// "implicit" rather than "mobilized" -- putting removals after the fills makes
// the client's apply order agree with the authority's drain order for free.
//
// A reader that stops after the brick array simply ignores it, so an old client
// degrades to the pre-9c behaviour (bricks stay mobilized) rather than
// desynchronising on a length mismatch.
//
// --- THE BASIN-LEDGER SECTION (water re-architecture Phase 2) ---------------
//
// A third trailing section: `u32 tag, u32 version, u32 count`, then `u64
// basinId, i64 delta` per basin.
//
// WHY THIS CHANNEL RATHER THAN A NEW ONE. The plan's §5 promise is that
// "nothing particle-shaped crosses the wire; scalars replicate", and the
// scalars are TINY -- 16 bytes per lake that moved, against 524 bytes per water
// brick. Standing up a second multicast for a payload three orders of magnitude
// smaller than the one already going out at the same 5 Hz would be a second
// thing to keep in step for no bytes saved. It rides along.
//
// WHY IT IS SAFE TO APPEND. The two sections above already establish the
// pattern and the reader already honours it: `ApplyReplicatedWaterDiffs` reads
// the removal count with `if (R.u32(...))` and treats absence as "a batch from
// a build that predates this section". A third section inherits that exactly --
// an old client stops after the removals and is merely a client whose lakes do
// not move, which is the pre-Phase-2 behaviour rather than a desync.
//
// THE TAG IS NOT DECORATION. Sections 1 and 2 are positional, which was fine
// while there were two of them and it is not fine at three: a new client
// reading an old batch that happens to have trailing bytes would take them for
// a basin count. The tag makes a section that is not this section
// unmistakable, and the version beside it makes a FOURTH section addable
// without this argument having to be had again.
//
// CAPPED, and capped at rows rather than at bytes: 256 basins is 4 KB out of
// the 32 KB round, and there is no world in which more than 256 lakes change
// level in 200 ms for any reason other than a bug. The remainder waits, exactly
// as deferred fill does.
constexpr uint32 kBasinSectionTag = 0x44534E42; // "BNSD" little-endian
constexpr uint32 kBasinSectionVersion = 1;
constexpr size_t kMaxBasinRowsPerBroadcast = 256;

void SerializeWaterDiffs(const std::vector<vxc::BrickKey>& Keys, const std::vector<vxc::BrickKey>& Removals,
                          const std::vector<std::pair<uint64_t, int64_t>>& BasinRows,
                          const vxc::WaterCA& CA, int32 MaxBytes, TArray<uint8>& OutBytes,
                          int32& OutEncodedBrickCount, size_t& OutConsumedKeyCount,
                          int32& OutEncodedRemovalCount, size_t& OutConsumedRemovalCount,
                          int32& OutEncodedBasinCount, size_t& OutConsumedBasinCount)
{
	std::vector<uint8_t> Bytes;
	vxc::ByteWriter W(Bytes);
	W.u32(0); // patched below once the final encoded count is known
	uint32_t Encoded = 0;
	size_t Consumed = 0;
	constexpr size_t EntrySize = 12 + size_t(vxc::WaterBrick8::kCells); // i32 x,y,z + 512 raw fill bytes
	constexpr size_t RemovalSize = 12;                                  // i32 x,y,z

	// RESERVE FOR THE REMOVALS FIRST. A key removal is 44x cheaper than a brick
	// and it is the message that ENDS an obligation, so letting a flood of fill
	// crowd it out for round after round would leave clients holding bricks the
	// authority has already given back -- the leak this item exists to close,
	// reappearing as a replication backlog. Fill defers gracefully (it is
	// re-sent next round because the brick is still dirty); a removal is drained
	// from its queue exactly once, so it must not be dropped.
	constexpr size_t BasinRowSize = 16; // u64 id + i64 delta
	const size_t RemovalBudget = std::min(Removals.size(), size_t(64));
	// RESERVE FOR THE BASINS TOO, and for the same reason as the removals: a
	// basin row is 33x cheaper than a brick, and a lake whose level moved is a
	// GAMEPLAY fact -- it is what a returning player sees from the ridge -- so
	// letting a flood of fill crowd it out round after round would leave clients
	// looking at a lake the authority drained minutes ago. 12 bytes of section
	// header on top of the rows.
	const size_t BasinBudget = std::min(BasinRows.size(), kMaxBasinRowsPerBroadcast);
	const size_t Reserved = RemovalBudget * RemovalSize + 4 + BasinBudget * BasinRowSize +
	                        (BasinBudget > 0 ? 12u : 0u);
	const size_t FillCap =
	    size_t(MaxBytes) > Reserved ? size_t(MaxBytes) - Reserved : size_t(MaxBytes);

	for (; Consumed < Keys.size(); ++Consumed)
	{
		const vxc::BrickKey& K = Keys[Consumed];
		const vxc::WaterBrick8* Brick = CA.findBrick(K);
		if (!Brick)
		{
			continue; // emptied since being marked dirty -- nothing to send, but fully handled
		}
		if (Encoded > 0 && Bytes.size() + EntrySize > FillCap)
		{
			break; // cap reached -- this (and later) keys wait for next round
		}
		W.i32(K.x);
		W.i32(K.y);
		W.i32(K.z);
		for (int z = 0; z < vxc::WaterBrick8::kEdge; ++z)
			for (int y = 0; y < vxc::WaterBrick8::kEdge; ++y)
				for (int x = 0; x < vxc::WaterBrick8::kEdge; ++x) W.u8(Brick->get(x, y, z));
		++Encoded;
	}

	Bytes[0] = uint8_t(Encoded);
	Bytes[1] = uint8_t(Encoded >> 8);
	Bytes[2] = uint8_t(Encoded >> 16);
	Bytes[3] = uint8_t(Encoded >> 24);

	const size_t RemovalCountOffset = Bytes.size();
	W.u32(0); // patched below
	uint32_t EncodedRemovals = 0;
	size_t ConsumedRemovals = 0;
	for (; ConsumedRemovals < Removals.size(); ++ConsumedRemovals)
	{
		if (Bytes.size() + RemovalSize > size_t(MaxBytes))
		{
			break;
		}
		W.i32(Removals[ConsumedRemovals].x);
		W.i32(Removals[ConsumedRemovals].y);
		W.i32(Removals[ConsumedRemovals].z);
		++EncodedRemovals;
	}
	Bytes[RemovalCountOffset + 0] = uint8_t(EncodedRemovals);
	Bytes[RemovalCountOffset + 1] = uint8_t(EncodedRemovals >> 8);
	Bytes[RemovalCountOffset + 2] = uint8_t(EncodedRemovals >> 16);
	Bytes[RemovalCountOffset + 3] = uint8_t(EncodedRemovals >> 24);

	// --- THE BASIN-LEDGER SECTION -------------------------------------------
	//
	// EMITTED ONLY WHEN THERE IS SOMETHING IN IT, so a world with no lakes
	// moving puts not one extra byte on the wire and an old client sees a batch
	// byte-identical to the one it saw before Phase 2.
	uint32_t EncodedBasins = 0;
	size_t ConsumedBasins = 0;
	if (!BasinRows.empty())
	{
		W.u32(kBasinSectionTag);
		W.u32(kBasinSectionVersion);
		const size_t BasinCountOffset = Bytes.size();
		W.u32(0); // patched below
		for (; ConsumedBasins < BasinRows.size(); ++ConsumedBasins)
		{
			if (EncodedBasins >= kMaxBasinRowsPerBroadcast ||
			    Bytes.size() + BasinRowSize > size_t(MaxBytes))
			{
				break;
			}
			W.u64(BasinRows[ConsumedBasins].first);
			W.u64(uint64_t(BasinRows[ConsumedBasins].second)); // two's complement, exact
			++EncodedBasins;
		}
		Bytes[BasinCountOffset + 0] = uint8_t(EncodedBasins);
		Bytes[BasinCountOffset + 1] = uint8_t(EncodedBasins >> 8);
		Bytes[BasinCountOffset + 2] = uint8_t(EncodedBasins >> 16);
		Bytes[BasinCountOffset + 3] = uint8_t(EncodedBasins >> 24);
	}

	OutBytes.SetNumUninitialized((int32)Bytes.size());
	if (!Bytes.empty())
	{
		FMemory::Memcpy(OutBytes.GetData(), Bytes.data(), Bytes.size());
	}
	OutEncodedBrickCount = int32(Encoded);
	OutConsumedKeyCount = Consumed;
	OutEncodedRemovalCount = int32(EncodedRemovals);
	OutConsumedRemovalCount = ConsumedRemovals;
	OutEncodedBasinCount = int32(EncodedBasins);
	OutConsumedBasinCount = ConsumedBasins;
}
} // namespace

// Baked lakes with their own fine-tile source (watershed plan work item 4).
//
// WHY THIS OWNS A SECOND SAMPLER instead of borrowing the terrain's. The fine
// tier is streamed by FVoxelFineTileStreamer, which lives inside
// FVoxelWorldImpl -- a struct defined only in VoxelWorldSubsystem.cpp -- and
// keeps its vxc::FineTileSampler private behind an FRWLock. Reaching it would
// mean a public accessor on a subsystem this file already documents as
// belonging to someone else, and the same reasoning that made this file build
// its own Amplifier applies unchanged.
//
// THE DUPLICATION IS CHEAPER THAN IT LOOKS, which is what makes it acceptable
// rather than merely convenient. vxc::FineTileSampler decodes lazily, ONE
// 256x256 block at a time, and a LakeSampler only ever reads the blocks
// covering a basin's bbox -- the survey's median basin is 0.59 ha, about 1,700
// fine pixels. So this holds the tile headers and the handful of blocks the
// lakes near the player actually occupy, not the 201 MB lattice.
//
// The path layout is NOT restated here: vxc::formatFineTileCacheKey is the
// same function FVoxelFineTileStreamer::LocalPathFor uses, so the two cannot
// drift, and CoarseTileForWorldMm is its addressing rule rather than a second
// copy of the arithmetic.
class FLakeWaterSampler final : public vxc::IWaterSampler
{
public:
	FLakeWaterSampler(uint64 InSeed, FString InRoot, std::string InProviderId)
		: Tiles(InSeed), Lakes(Tiles), Rivers(Tiles), Both(Lakes, Rivers),
		  Root(MoveTemp(InRoot)), ProviderId(MoveTemp(InProviderId))
	{
		Tiles.setDecompressor(VoxelEarth::GetFineTileDecompressor());
	}

	// LAKES AND RIVERS AS ONE QUERY (watershed plan §5.1). Both samplers read
	// the SAME `Tiles`, so one EnsureTileFor covers both and there is no state
	// in which the lake half and the river half disagree about which tiles are
	// resident -- which is the failure the sheet half was already careful about.
	//
	// The composition is vxc::CompositeWaterSampler rather than a max() written
	// here, for the reason implicitWaterFill is in lakes.h rather than inline:
	// the binding site and the tests must not be able to express the rule
	// differently. It takes the HIGHER datum where they overlap; they should
	// not overlap at all (the bake writes the plane dry inside registered
	// basins) but taking the lower would drain a lake into the river feeding it.
	int32_t waterSurfaceMmAtVoxel(int64_t vx, int64_t vy) override
	{
		EnsureTileFor(vx, vy);
		return Both.waterSurfaceMmAtVoxel(vx, vy);
	}

	// Work item 5's sheet half. Same load-then-ask shape as the column query
	// above, so a sheet and a voxel disc over the same lake can never be reading
	// two different tile sets.
	const std::vector<vxc::BasinEntry>* basinsForTile(int32_t tx, int32_t ty) override
	{
		EnsureTile(tx, ty);
		return Lakes.basinsForTile(tx, ty);
	}
	const std::vector<uint8_t>* extentMaskFor(int32_t tx, int32_t ty, uint16_t id) override
	{
		EnsureTile(tx, ty);
		return Lakes.extentMaskFor(tx, ty, id);
	}
	uint32_t tilePixels() const override { return Tiles.tileSize(); }
	int32_t pixelSizeMm() const override { return Tiles.pixelSizeMm(); }

	uint64 TilesLoaded() const { return Loaded; }
	uint64 TilesMissing() const { return Missing.Num(); }
	uint64 TilesRefused() const { return Refused; }
	size_t ResidentMasks() const { return Lakes.residentMaskCount(); }
	size_t ResidentWaterBlocks() const { return Rivers.residentBlockCount(); }
	// Non-zero means a river's bytes were there and did not decode, which on
	// screen is indistinguishable from "there is no river here". Surfaced so it
	// can be logged rather than inferred from an empty valley.
	uint64 RiverBlocksUnresolved() const { return Rivers.unresolvedBlocks(); }

	// The far-field river producer needs BOTH halves directly (Phase 4):
	// riverRibbonFillWet reads the raw depth raster through the tile sampler,
	// deliberately bypassing the per-pixel spline to answer "is this wet" from
	// the one sign bit that is already decoded, and riverRibbonResolveDatum
	// then calls RiverSampler::surfaceAtPixel at the centreline only.
	//
	// EXPOSING THESE IS NOT A SECOND WORLD. Both references are into the ONE
	// `Tiles` this class owns, which is the same object waterSurfaceMmAtVoxel
	// reads, so the ribbon cannot see a different water plane than the near
	// field does -- riverribbon.h's own probe measures that agreement at 0
	// disagreements in 5,480,281 cells and this is what keeps it true here.
	// Game-thread only, like every other method on this class.
	//
	// These are the IWaterSampler hook, not new API: a caller reaches them
	// through Impl->Water without knowing this class exists, and a world with
	// no fine tier answers nullptr through NullWaterSampler's default. That is
	// what keeps the ribbon actor free of a downcast -- UE modules build with
	// /GR- (bUseRTTI defaults false), so dynamic_cast is not available here and
	// a static_cast onto the wrong sampler would be silent memory corruption.
	vxc::FineTileSampler* ribbonTiles() override { return &Tiles; }
	vxc::RiverSampler* ribbonRivers() override { return &Rivers; }

	// --- the Phase 2 ledger seam (voxelcore/lakes.h IBasinDatumSource) -------
	//
	// Three one-liners, and all three are forwards rather than new behaviour:
	// the composite already routes the datum question to the lake half, and
	// these let the host bind a ledger to it without knowing this class exists
	// (same /GR- argument as ribbonTiles above).
	int32_t basinDatumMm(int32_t tx, int32_t ty, const vxc::BasinEntry& baked) override
	{
		EnsureTile(tx, ty);
		return Both.basinDatumMm(tx, ty, baked);
	}
	void setBasinDatumSource(vxc::IBasinDatumSource* Source) override
	{
		Both.setBasinDatumSource(Source);
	}
	void invalidateBasinDatumMemo() override { Both.invalidateBasinDatumMemo(); }

private:
	// Loads the fine tile under this voxel column if it is not already
	// resident. A tile that is absent or refused is remembered, so a world
	// with no fine tier costs one stat() per tile for the whole session rather
	// than one per query.
	void EnsureTileFor(int64_t vx, int64_t vy)
	{
		const vxc::TileCoord T = FVoxelFineTileStreamer::CoarseTileForWorldMm(
			vx * vxc::kVoxelSizeMm, vy * vxc::kVoxelSizeMm);
		EnsureTile(T.x, T.y);
	}

	void EnsureTile(int32_t Tx, int32_t Ty)
	{
		const vxc::TileCoord T{Tx, Ty};
		if (Tiles.findTile(T.x, T.y) != nullptr)
		{
			return;
		}
		const uint64 Key = (uint64(uint32(T.x)) << 32) | uint64(uint32(T.y));
		if (Missing.Contains(Key))
		{
			return;
		}
		const std::string CacheKey = vxc::formatFineTileCacheKey(ProviderId, Tiles.seed(), T.x, T.y);
		const FString Path = FPaths::Combine(Root, FString(CacheKey.c_str()) + TEXT(".vxtl"));
		vxc::FineError Err = vxc::FineError::kNone;
		if (!Tiles.loadTileFile(std::filesystem::path(*Path), &Err))
		{
			Missing.Add(Key);
			// A tile that is simply not baked is expected and silent. Bytes
			// that ARE there and do not parse are not: that is a corrupt or
			// foreign-version tile, and silently treating it as "no lake here"
			// is how a world loses its water without anyone noticing.
			if (Err != vxc::FineError::kFileUnreadable)
			{
				++Refused;
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("Lake tier: fine tile (%d,%d) at %s was REFUSED (%s). Its lakes will be absent."),
				       T.x, T.y, *Path, ANSI_TO_TCHAR(vxc::fineErrorName(Err)));
			}
			return;
		}
		++Loaded;
	}

	// DECLARATION ORDER IS LOAD-BEARING: Lakes and Rivers borrow Tiles, and
	// Both borrows Lakes and Rivers. Reordering these members reorders their
	// construction and binds a reference to an object that does not exist yet.
	vxc::FineTileSampler Tiles;
	vxc::LakeSampler Lakes;
	vxc::RiverSampler Rivers;
	vxc::CompositeWaterSampler Both;
	FString Root;
	std::string ProviderId;
	TSet<uint64> Missing;
	uint64 Loaded = 0;
	uint64 Refused = 0;
};

// Resolves -VoxelFineTileDir / -VoxelFineTileProviderId with the SAME
// precedence UVoxelWorldSubsystem::Initialize uses (switch wins, then
// DefaultFineTileDir in DefaultGame.ini, then nothing). Restated here rather
// than shared because the alternative is a public accessor on another
// subsystem; the rule is three lines and the ini keys are the same two
// strings, and a divergence shows up immediately as "terrain is fine, water
// says there are no lakes".
std::unique_ptr<vxc::IWaterSampler> MakeWaterSampler(uint64 Seed)
{
	FString Dir;
	if (!FParse::Value(FCommandLine::Get(), TEXT("VoxelFineTileDir="), Dir) && GConfig)
	{
		GConfig->GetString(TEXT("/Script/VoxelEarth.VoxelWorldSubsystem"),
		                   TEXT("DefaultFineTileDir"), Dir, GGameIni);
	}
	if (Dir.IsEmpty())
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Lake tier DISABLED: no -VoxelFineTileDir. Baked lakes need the fine tier "
		            "(the basin table lives in the .vxtl); caverns and the ocean are unaffected."));
		return std::make_unique<vxc::NullWaterSampler>();
	}
	if (FPaths::IsRelative(Dir))
	{
		Dir = FPaths::Combine(FPaths::ProjectContentDir(), Dir);
		FPaths::CollapseRelativeDirectories(Dir);
	}
	FString ProviderId;
	if (!FParse::Value(FCommandLine::Get(), TEXT("VoxelFineTileProviderId="), ProviderId) && GConfig)
	{
		GConfig->GetString(TEXT("/Script/VoxelEarth.VoxelWorldSubsystem"),
		                   TEXT("DefaultFineTileProviderId"), ProviderId, GGameIni);
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Baked water tier ENABLED: root=%s provider=%s seed=%llu. Lakes come from the basin "
	            "table (bake_ver 8) and rivers from the water plane (bake_ver 9); a tile baked "
	            "before either carries it not, and answers dry for that half alone."),
	       *Dir, *ProviderId, (unsigned long long)Seed);
	return std::make_unique<FLakeWaterSampler>(Seed, Dir, std::string(TCHAR_TO_UTF8(*ProviderId)));
}

// FVoxelWaterImpl -- the voxel-core side of the subsystem, defined only here
// so VoxelWaterSubsystem.h (UHT-parsed) never sees a voxel-core header (same
// pattern as FVoxelWorldImpl in VoxelWorldSubsystem.cpp).
struct FVoxelWaterImpl
{
	explicit FVoxelWaterImpl(UVoxelWorldSubsystem& InTerrain)
		: Terrain(InTerrain)
		, Tiles(InTerrain.GetSeed())
		, Amp(InTerrain.GetSeed(), Tiles)
		, Water(MakeWaterSampler(InTerrain.GetSeed()))
		, Mob(
			  // The implicit static flood field (C7, docs/cavern-design.md SS5.1):
			  // worldgen-owned, deterministic, ZERO storage. caverns.h's
			  // cavernFloodedAt is exactly half the predicate -- "is this below
			  // the column's flood level" -- and WaterMobilizer applies the other
			  // half (is the cell actually open air) itself, so this callback
			  // stays a pure worldgen query.
			  // BAKED LAKES JOIN HERE AND NOWHERE ELSE (watershed plan §5.1,
			  // work item 4). A baked lake datum has the SAME SHAPE as the
			  // cavern flood level this callback already answers -- "water
			  // fills open air below this millimetre level in this column" --
			  // so the whole lake feature is one more term in this expression
			  // and NOT a second mechanism. Everything downstream is
			  // inherited: unmobilized lake water is a wall, digging the shore
			  // fires NotifyTerrainVoxelsCleared -> mobilizeEditRegion, fill
			  // replicates as diffs, the ledger audits to zero, and
			  // IsUnderwaterAtWorld (work item 1) starts answering for lakes
			  // with no second predicate written anywhere.
			  //
			  // vxc::implicitWaterFill is the composition itself, in
			  // voxelcore/lakes.h rather than inline here, so the binding site
			  // and tests/test_lakes.cpp cannot express it differently. It is
			  // also what adds the PARTIAL TOP FILL: the topmost water voxel
			  // carries surfaceMm's sub-voxel remainder, so the surface sits
			  // AT the datum instead of snapping to the 10 cm lattice.
			  // ...AND THE OCEAN JOINS IN THE SAME PLACE (watershed plan §6.4,
			  // work item 8). The sea is a lake whose datum is kSeaLevelMm and
			  // whose extent is "every column whose worldgen ground lies below
			  // it", so it is `vxc::implicitWaterDatumMm` composing with the
			  // baked surface by max() and NOT a fourth branch here. What that
			  // buys, all inherited and none of it written: unmobilized sea is
			  // a wall to the CA, a breach funnels through the SAME
			  // NotifyTerrainRegionEdited -> mobilizeEditRegion the lakes use,
			  // the ledger audits to zero, and the retired Reservoir v0 --
			  // which was a cell pinned at 255 forever, could not tell a dug
			  // pit from the sea, and poured unbounded water into a seabed the
			  // CA thought was dry air -- is gone. voxel-core's
			  // tests/test_ocean.cpp measures all three.
			  //
			  // GroundMmAt IS THE WORLDGEN AMPLIFIED SURFACE, NOT THE EDITED
			  // OVERLAY, and here that is not a nicety, it is the whole fix: a
			  // pit a player digs into land does not lower its column's
			  // worldgen ground, so the ocean term keeps answering "no sea
			  // here" however deep the pit goes. Same rule, same reason, as
			  // IsUnderwaterAtWorld's own seabed test (work item 1).
			  // THE BODY IS A MEMBER FUNCTION, NOT A LAMBDA BODY, and that is
			  // not tidiness. VerifyWaterDiskRoundTrip builds a SECOND
			  // WaterMobilizer over "the same implicit-flood / terrain-solidity
			  // callbacks" to prove the on-disk blob reloads to the same digest,
			  // and it did that by restating this expression. Two copies of the
			  // implicit field is two answers to "is this cell a wall", and the
			  // round-trip verifier would report the difference as a
			  // SERIALIZATION bug. One body, both callers -- see
			  // ImplicitFillAtVoxel below.
			  [this](int64_t vx, int64_t vy, int64_t vz) -> uint8_t
			  { return ImplicitFillAtVoxel(vx, vy, vz); },
			  [this](int64_t vx, int64_t vy, int64_t vz) -> vxc::MaterialId
			  { return Terrain.IsSolidAtVoxel(vx, vy, vz) ? vxc::MAT_ROCK : vxc::MAT_AIR; })
		// NOT the bare terrain query: makeSolidFn() layers the implicit-water
		// WALL on top, which is what makes the implicit/CA ownership partition
		// structural rather than a matter of call ordering (waterca.h).
		, CA(Mob.makeSolidFn())
	{
		// DEMOTION PRESSURE AT THE HIGH-WATER MARK (§6.5.5, work item 9b's hook
		// wired to 9c's sweep). advanceFront calls this at most once per call and
		// only while at the ceiling, so a world under its ceiling pays nothing.
		// The point of the hook rather than a demotion after the fact is that
		// whatever it frees is visible to the rest of that same call -- "reclaim,
		// then refuse" instead of "refuse, then reclaim next tick".
		//
		// AUTHORITY ONLY, enforced here because voxel-core cannot. A client's CA
		// is a replication mirror whose fill lags the authority, so running the
		// exact predicate against it would refuse where the authority accepted
		// and the two would diverge permanently. In practice this is unreachable
		// on a client (advanceFront is only called from StepFixed, which Tick
		// runs only when NetMode != NM_Client) -- the guard is belt and braces
		// for anyone who later finds another caller for the front.
		//
		// `this` is safe to capture: FVoxelWaterImpl lives in a TUniquePtr that
		// is never moved, and Mob/CA are members, so the lambda cannot outlive
		// them.
		Mob.setCeilingRelief(
			[this]
			{
				if (!bAuthority)
				{
					return;
				}
				const int32 Budget = CVarVoxelWaterCeilingReliefBudget.GetValueOnGameThread();
				if (Budget > 0)
				{
					Mob.demoteBudgeted(CA, size_t(Budget));
				}
			});

		// THE FRONT GATE (work item 9a) IS DELIBERATELY LEFT UNINSTALLED, and
		// that is a measured decision rather than an omission -- see this
		// change's commit message and the four C8e tests in voxel-core.
		//
		// The gate is a predicate that freezes a reach the AUTHORITY has decided
		// to dry out by changing the datum (§6.3.3). Nothing in this engine
		// drives such a decision yet: there is no datum-override registry and no
		// caller that would populate one. The obvious candidate policy -- a
		// cooldown that refuses to re-mobilize a brick just demoted, to stop the
		// return path thrashing against the front -- was built and measured, and
		// it is NOT shippable: its effect is non-monotonic in the cooldown length
		// (on the reference breach, 8 steps left the runaway untouched, 10 steps
		// made peak mobilization roughly DOUBLE the ungated run, and 16 steps
		// fixed it) and it collapsed to no effect at all on a breach four times
		// the size. That is a knife-edge in a chaotic response surface, not a
		// policy, and layering it over the primitive would have hidden the real
		// finding: what breaks a large breach is kMaxHydrostaticComponentCells,
		// which sits upstream of every lever 9a/9b/9c provide.
		//
		// The seam is one line (`Mob.setFrontGate(...)`) whenever a real freeze
		// owner appears. Until then the mechanism stays proven and unused rather
		// than used and unjustified.
	}

	UVoxelWorldSubsystem& Terrain;

	// EVERY WORLDGEN FACT THE WATER PATH USES COMES FROM THE TERRAIN'S OWN
	// AMPLIFIER, not from `Amp` below. That used to read "the ground under a
	// baked lake must", with the cavern half explicitly left behind; the cavern
	// half joined it on 2026-08-04 and the paragraph below is why.
	//
	// `Amp` is built over a SyntheticTileSampler (see its comment): under
	// -VoxelTileDir it is amplifying a DIFFERENT WORLD from the one on screen.
	// For cavern flood levels that was a known, tolerated inaccuracy until it
	// was measured. For a baked lake it is fatal: the datum comes from the
	// BAKED surface, so
	// bounding it below with the synthetic surface would put the water tens or
	// hundreds of metres away from its own bed -- buried in rock, or a sheet
	// hanging in the air -- and neither would look like a bug in the water
	// code.
	//
	// UVoxelWorldSubsystem::GetWorldgenSurfaceAndCavernFloodMm is the terrain's
	// own amplifier column, documented as a game-thread query, and it now
	// answers BOTH halves. So does this memo.
	//
	// THE CAVERN HALF FOLLOWED THE LAKE HALF OFF `Amp` ON 2026-08-04, and the
	// paragraph above used to end "fixing that is the column-accessor follow-up
	// this file has been asking for and is not this change". It became this
	// change the day the tolerated inaccuracy was measured: at the owner's
	// camera the synthetic surface is 638.451 m where the renderer draws 77.6 m,
	// so `Amp`'s cavern flood levels stood at 606.166 m and the near-field sweep
	// offered 511 bricks of water 528 m above the ground -- a 52 m disc, in open
	// sky, following the camera. Nothing about that was specific to caverns
	// being approximate; the two Amplifiers simply describe different planets.
	//
	// ONE CALL FOR BOTH is not an optimisation, it is the invariant: a ground
	// from here and a flood level from anywhere else is the defect, restated.
	//
	// MEMOISED PER COLUMN, and the memo is only worth anything because
	// BuildWaterFillPad sweeps z INNERMOST. That was an assumption in this
	// comment and a falsehood in the code until 2026-08-03 -- the pad was built
	// z-outermost, so this memo, LakeSampler's and BOTH Amplifier::columnCached
	// memos missed on every one of the 1,000 cells in a brick. See
	// BuildWaterFillPad for the measurement. Do not reorder that loop nest
	// without reading it.
	//
	// Note the miss is not merely an amplifier column: the accessor also fires
	// FVoxelFineTileStreamer::RequestFootprint, so a missing memo puts a
	// streaming request on the game thread per voxel rather than per column.
	//
	// WHAT THIS COSTS THAT THE OLD SHAPE DID NOT, stated rather than discovered:
	// a DRY column used to answer the ImplicitFn from `Amp` alone and never
	// reach the terrain. It now pays one terrain column, memoised, on its first
	// voxel. That is one per column, not one per voxel, and it is unavoidable --
	// "is there a cavern under this column" cannot be answered by the wrong
	// world. UNMEASURED in a running editor (see this change's report).
	void EnsureWorldgenColumn(int64_t vx, int64_t vy)
	{
		if (bGroundMemoValid && vx == GroundMemoVx && vy == GroundMemoVy)
		{
			return;
		}
		int32_t SurfaceMm = 0;
		int32_t FloodZMm = INT32_MIN;
		// A false return is "no world yet" (transient Entry map). The outputs
		// are already the safe pair -- ground 0, no cavern -- so there is
		// nothing to branch on: this must not invent water either way.
		Terrain.GetWorldgenSurfaceAndCavernFloodMm(vx, vy, SurfaceMm, FloodZMm);
		GroundMemoMm = SurfaceMm;
		CavernFloodMemoMm = FloodZMm;
		GroundMemoVx = vx;
		GroundMemoVy = vy;
		bGroundMemoValid = true;
	}
	// Absolute mm of the amplified surface. Exact now rather than rounded off a
	// UU double: the accessor hands back the amplifier's own `surfaceMm`, which
	// is what `GetSurfaceHeightUU` divided by 10 to make the double this used to
	// round back. Same number, one conversion fewer.
	int32_t GroundMmAt(int64_t vx, int64_t vy)
	{
		EnsureWorldgenColumn(vx, vy);
		return GroundMemoMm;
	}
	// vxc::CavernColumn::floodZMm for this column, INT32_MIN where there is no
	// site in reach.
	int32_t CavernFloodMmAt(int64_t vx, int64_t vy)
	{
		EnsureWorldgenColumn(vx, vy);
		return CavernFloodMemoMm;
	}
	int64_t GroundMemoVx = 0, GroundMemoVy = 0;
	int32_t GroundMemoMm = 0;
	int32_t CavernFloodMemoMm = INT32_MIN;
	bool bGroundMemoValid = false;

	// THE IMPLICIT STATIC WATER FIELD (C7, docs/cavern-design.md SS5.1;
	// watershed plan §5.1 for baked lakes and §6.4 for the ocean), as one
	// function because two callers need the identical answer -- see the note at
	// the Mob initialiser above.
	//
	// The composition itself is voxel-core's (`cavernWaterAt`,
	// `implicitWaterDatumMm`, `implicitWaterFill`) so the binding site and
	// tests/test_lakes.cpp cannot express it differently.
	uint8_t ImplicitFillAtVoxel(int64_t vx, int64_t vy, int64_t vz)
	{
		const int32_t CavernFloodMm = CavernFloodMmAt(vx, vy);
		// THE LAKE TERM IS LEDGER-ADJUSTED, and there is no code here that says
		// so, which is the point. `waterSurfaceMmAtVoxel` reaches
		// LakeSampler::surfaceAtPixel, which asks `basinDatumMm`, which is the
		// one seam the basin ledger binds to (voxelcore/lakes.h
		// IBasinDatumSource). So a credited basin rises in the near field with
		// ZERO change to this function -- and, more importantly, it rises by
		// the same number the far-field sheet uses, because both went through
		// that seam rather than each applying a delta of its own.
		//
		// The name stays `BakedMm` because that is still what it is when no
		// ledger is bound, which is every world without a fine tier.
		int32_t BakedMm = Water->waterSurfaceMmAtVoxel(vx, vy);

		// LATERAL FILL AT VOXEL RESOLUTION (-VoxelWaterLateralFillPx=<n>, off by
		// default).
		//
		// WHAT IS ALREADY RIGHT, so this is not what it looks like. The fill
		// below is ALREADY per-voxel against the AMPLIFIED 10 cm ground:
		// GroundMmAt is ground #3, and implicitWaterFill even carries the
		// topmost voxel's sub-voxel remainder so the surface sits AT the datum
		// instead of snapping to the lattice. The waterline is not the problem.
		//
		// WHAT IS WRONG is the DOMAIN. waterSurfaceMmAtVoxel answers kNoWaterMm
		// outside the baked wet mask, and that mask is one decision per FINE
		// PIXEL -- 1,875 mm (tile_codec.PIXEL_SIZE_MM[16]) against a 100 mm
		// voxel. So the per-voxel waterline is computed inside cells the bake
		// called wet and NOT COMPUTED AT ALL one cell over, and the edge snaps to
		// a 1.875 m boundary. Flying it, the owner's words: "the magenta water
		// volumes just stop in very blocky, multi meter sized, square edges -
		// expectation is that water fills the river bed and meets the bank at the
		// 10cm per voxel scale."
		//
		// So this extends the DOMAIN of the datum, not the fill rule. A dry
		// column takes the surface of the NEAREST wet cell within n pixels and
		// then faces the same per-voxel test it always did.
		//
		// NEAREST, NOT HIGHEST, and that distinction is a bug I already wrote
		// once: a max over the search area lets a column inherit a level from up
		// to n pixels UPSTREAM, which on a descending bed fills a wedge that
		// thickens downstream -- more multi-metre slabs on steep reaches,
		// manufactured by the fix meant to help. Water beside a bank takes the
		// level of the water beside it.
		//
		// IT CANNOT RUN AWAY: a column is only wet while its amplified ground is
		// below that surface, so a bank higher than the water stops it dead and
		// the reach of the fill is a fact about the terrain rather than a radius
		// anyone tuned. The radius bounds the SEARCH, not the flood.
		//
		// OFF BY DEFAULT because this is the CA's hottest query and it adds up to
		// 8n sampler calls to a dry miss. Turn it on to judge the shape; measure
		// before considering it for default.
		if (BakedMm == vxc::kNoWaterMm)
		{
			static const int32 LateralPx = []
			{
				int32 N = 0;
				FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterLateralFillPx="), N);
				return FMath::Clamp(N, 0, 32);
			}();
			if (LateralPx > 0)
			{
				constexpr int64_t kPixelVoxels = 19; // 1875 mm / 100 mm, rounded up
				static const int64_t kDir[8][2] = {{1, 0}, {-1, 0}, {0, 1},  {0, -1},
				                                   {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
				for (int64_t Step = 1; Step <= LateralPx && BakedMm == vxc::kNoWaterMm; ++Step)
				{
					for (int D = 0; D < 8; ++D)
					{
						const int32_t N = Water->waterSurfaceMmAtVoxel(
							vx + kDir[D][0] * Step * kPixelVoxels,
							vy + kDir[D][1] * Step * kPixelVoxels);
						if (N == vxc::kNoWaterMm) continue;
						// Lowest within the ring: the ring can straddle two
						// levels on a slope, and the lower one cannot cover
						// ground the higher one would not have.
						if (BakedMm == vxc::kNoWaterMm || N < BakedMm) BakedMm = N;
					}
				}
			}
		}
		// THE z GUARD IS A PERF GUARD AND IT IS LOAD-BEARING. This callback is
		// the CA's HOTTEST query -- makeSolidFn() runs it for every
		// genuinely-open-air cell the CA touches.
		//
		// The guard is exact, not a heuristic: the ocean's datum is
		// kSeaLevelMm, and waterFillUnits' remainder for a voxel whose BOTTOM
		// is at or above the datum is <= 0. So at or above kSeaLevelVoxelZ the
		// ocean term provably contributes nothing.
		const bool bOceanPossible = bImplicitOcean && vz < vxc::kSeaLevelVoxelZ;
		if (CavernFloodMm == INT32_MIN && BakedMm == vxc::kNoWaterMm && !bOceanPossible)
		{
			return 0;
		}
		const int32_t GroundMm = GroundMmAt(vx, vy);
		// CAVERN WATER IS BOUNDED ABOVE BY THE GROUND, which is `cavernWaterAt`
		// rather than the bare `cavernFloodedAt` this used to call. That
		// predicate is only half of the pair caverns.h documents ("... &&
		// materialAt(col, vz) == MAT_AIR"); the other half here is the
		// mobilizer's terrain SOLIDITY query, and open sky is not solid. So
		// above the ground the pair degenerated to "below the flood level" and
		// a flood level above this column's ground filled open air. See
		// cavernWaterAt's comment for why that is reachable even when both
		// halves DO come from the same world.
		if (vxc::cavernWaterAt(CavernFloodMm, vz, int64_t(GroundMm)))
		{
			return 255;
		}
		const int32_t SurfMm =
			bOceanPossible ? vxc::implicitWaterDatumMm(BakedMm, GroundMm) : BakedMm;
		if (SurfMm == vxc::kNoWaterMm)
		{
			return 0;
		}
		return vxc::implicitWaterFill(vz, GroundMm, SurfMm, false);
	}

	// OUR OWN worldgen sampler, not UVoxelWorldSubsystem's. That subsystem is
	// another agent's file and exposes no column/cavern accessor, so — exactly
	// as AVoxelClipmapActor already does for terrain height — we build a second
	// Amplifier over the same seed. Worldgen is a pure function of (seed, tile
	// sampler), so this is bit-identical to the terrain's own for every run
	// that uses the synthetic sampler (the default).
	//
	// CAVEAT, and the reason for the warning logged in OnWorldBeginPlay: a run
	// launched with -VoxelTileDir uses a real tile-grid sampler over there and
	// this synthetic one over here, so the surfaces disagree and cavern flood
	// levels are computed against the wrong terrain.
	//
	// THAT CAVEAT CAME DUE ON 2026-08-04 AND IS NO LONGER TOLERATED ON THE
	// WATER PATH. Measured at the owner's camera: 638.451 m of ground here
	// against 77.6 m on screen, so flood levels at 606.166 m and a 52 m disc of
	// water 528 m in the air. The implicit field (ImplicitFillAtVoxel) and the
	// near-field sweep (RefreshImplicitWater) now both take the cavern flood
	// level from UVoxelWorldSubsystem::GetWorldgenSurfaceAndCavernFloodMm --
	// the column accessor the paragraph above was waiting for -- alongside the
	// ground the lake half already took from there.
	//
	// WHAT STILL READS THIS AMPLIFIER, so it is a list and not a surprise:
	// GetCavernFloodZUU and FindFloodedCavernNear, the -VoxelCavernShot camera
	// placement tools. They answer "where is a flooded cavern to photograph",
	// nothing renders or mobilises from them, and moving them needs a full
	// ColumnSample (materialAt, not just the flood level) across the same
	// boundary. On a baked run they will point at the synthetic world's caverns
	// -- which is the same defect, still open, in a tool rather than in the
	// world.
	vxc::SyntheticTileSampler Tiles;
	vxc::Amplifier Amp;

	// BAKED LAKES (watershed plan work item 4). An interface pointer, not a
	// LakeSampler by value, because "no fine tier" is a SUPPORTED configuration
	// and not a null check at every call: with no baked tiles this is a
	// NullWaterSampler answering kNoWaterMm everywhere, and the client behaves
	// exactly as it did before lakes existed.
	//
	// MakeWaterSampler below resolves -VoxelFineTileDir with the same precedence
	// UVoxelWorldSubsystem uses, so a run that has fine terrain has lakes and a
	// run that does not, does not -- there is no third state where the water and
	// the ground disagree about which world this is.
	//
	// MUST be declared before Mob: the ImplicitFn captures `this` and
	// dereferences this member on every voxel it is asked about.
	std::unique_ptr<vxc::IWaterSampler> Water;

	// DEBUG WATER MARKER (-VoxelWaterMarker=1). `Water` above is documented
	// GAME-THREAD ONLY -- every real sampler decodes lazily on query -- and
	// Amplifier::column runs on the MESHER WORKER POOL, so the terrain side
	// cannot borrow `Water` directly without racing its tile decode from many
	// threads at once. This wraps it behind a mutex; the lock is paid only by
	// the debug path, never by the near-field sweep.
	//
	// DECLARED AFTER `Water` so it is DESTROYED BEFORE it: it holds a raw
	// reference to the sampler it wraps. And it must outlive the terrain
	// amplifier's use of it, which is why Deinitialize clears the marker before
	// this struct goes away.
	std::unique_ptr<vxc::LockedWaterSampler> MarkerWater;

	// --- THE SCALAR HYDROLOGY AUTHORITY (water re-architecture Phase 2) ------
	//
	// A basin's wire datum is where the CLIMATE says the lake stands. These four
	// are what makes it move: an int64 volume per basin, the curve that turns
	// that volume into a level, and the seam through which the drawn water asks.
	//
	// DECLARATION ORDER IS LOAD-BEARING a fourth time in this struct, and the
	// chain is one-directional: BasinDatum borrows Basins, Basins borrows
	// BasinCapacity, BasinCapacity borrows BasinTerrain, BasinTerrain borrows
	// the FineTileSampler that `Water` above owns. Declared in that order, so
	// they are DESTROYED in the reverse of it and nothing is ever holding a
	// reference to something already gone. All four are declared AFTER `Water`
	// for the same reason.
	//
	// ALL FOUR ARE NULL unless the fine tier resolved (EnsureBasinLedger below),
	// because with no basin table there is nothing to keep a ledger of, and a
	// null here is the same supported "no fine tier" configuration `Water` is
	// already a NullWaterSampler for.
	std::unique_ptr<vxc::FineTileBasinTerrain> BasinTerrain;
	std::unique_ptr<vxc::ClientHypsometryProvider> BasinCapacity;
	std::unique_ptr<vxc::BasinLedger> Basins;
	std::unique_ptr<vxc::BasinLedgerDatumSource> BasinDatum;

	// Basins whose delta has moved since the last ~5 Hz broadcast, as packed
	// BasinId keys. A TSet, so a basin credited ten times in one window costs
	// one row on the wire -- the scalar's whole replication advantage is that
	// its size is a function of how many lakes changed, not of how much they
	// changed or how often.
	TSet<uint64> BasinDirtySinceLastBroadcast;
	// Session audit for the ledger's hand-offs, so a silent Phase 2 is
	// distinguishable from an inactive one.
	int64 BasinSpillUnitsRouted = 0;
	int64 BasinSpillUnitsRefunded = 0;
	int64 BasinReplicatedRows = 0;

	// --- Phase 3 sill-faucet intercept (see SetFluidSpillIntercept) ---------
	// While enabled, spill events whose baked outlet falls inside the box are
	// HELD here for the fluid host instead of routed into the graph. Each
	// entry carries the wall-clock second it was parked, so RouteBasinSpills
	// can refund anything a dead or stalled fluid host never drained -- held
	// units are OWED water, and the timeout is what keeps "owed" from decaying
	// into "lost".
	bool bFluidSpillInterceptEnabled = false;
	double FluidSpillBoxMinXUU = 0.0, FluidSpillBoxMinYUU = 0.0;
	double FluidSpillBoxMaxXUU = 0.0, FluidSpillBoxMaxYUU = 0.0;
	struct FFluidHeldSpill
	{
		vxc::BasinSpillEvent Event;
		double HeldSinceSeconds = 0.0;
	};
	std::vector<FFluidHeldSpill> FluidSpillHeld;
	int64 FluidSpillUnitsClaimed = 0;  // handed to the fluid host, session total
	int64 FluidSpillUnitsTimedOut = 0; // refunded by the grace-window flush

	// The routing-graph half of the hydrology blob, held from load until the
	// graph exists to apply it to.
	//
	// WHY IT HAS TO WAIT. The ledger can be restored the moment the world
	// begins play -- it is just numbers. The graph cannot: `Rivers` is null
	// until voxel.Water.Rivers is armed, and a RiverNetState blob is meaningless
	// without the base topology it was recorded against. So the bytes sit here
	// and MaybeArmRivers applies them the instant it has built a graph, which is
	// also the only moment at which the segment-count check in
	// restoreRoutingState means anything.
	//
	// CONSUMED EXACTLY ONCE. A second arm over different bounds must NOT get
	// these bytes: the graph would be a different graph, restoreRoutingState
	// would refuse it, and the refusal would be logged every time the player
	// toggled the cvar.
	std::vector<uint8_t> PendingHydroGraphBlob;

	// voxel.Water.ImplicitOcean, LATCHED (watershed plan §6.4). Declared before
	// Mob for the same reason Water is: the ImplicitFn reads it on every voxel
	// it is asked about.
	//
	// LATCHED RATHER THAN READ LIVE, and that is not a micro-optimisation. The
	// implicit field is what makes unmobilized water a WALL to the CA
	// (waterca.h's ownership partition), and mobilization is a ONE-WAY
	// surrender recorded in the savegame. A field that changed shape mid-tick
	// would let the CA walk into cells the mobilizer still owns, which is the
	// one thing the wall exists to forbid, and the symptom would be a ledger
	// shortfall rather than a crash. Tick re-reads it on a step boundary.
	bool bImplicitOcean = true;

	// MUST be declared before CA: makeSolidFn() hands the CA a callable that
	// captures the mobilizer, so the mobilizer has to outlive it.
	vxc::WaterMobilizer Mob;

	vxc::WaterCA CA;

	// One UWaterChunkComponent per rendered IMPLICIT (not-yet-mobilized) water
	// brick, keyed the same way ChunkComponents is. Kept separate from the CA's
	// map so mobilization is a clean handover: the implicit component is
	// destroyed and the CA's is created, and because implicitFillAt() returns 0
	// for a mobilized brick the two can never both draw the same water.
	TMap<VoxelCoords::FVoxelCoord, TObjectPtr<UWaterChunkComponent>> ImplicitChunkComponents;

	// Brick coordinate the implicit-water refresh last centred on, so a
	// stationary camera costs nothing. Set once the first refresh has run.
	VoxelCoords::FVoxelCoord LastImplicitCenterBrick = {0, 0, 0};
	bool bImplicitCenterValid = false;

	// The brick box the implicit sweep last covered. Held alongside the centre
	// (which GetImplicitWaterDiscUU and the drain's distance sort still want)
	// because the SWEEP is now a difference against this rather than a rebuild
	// from the centre. See voxelcore/waterwindow.h for why that is an identity
	// and not an approximation, and
	// docs/measurements/water-refresh-2026-08-05.txt for what it costs.
	vxc::WaterWindow LastImplicitWindow;

	// Per-COLUMN sweep results, keyed by brick column, for the columns the
	// window currently covers.
	//
	// THIS CACHE IS THE SAVING, not the region maths. The expensive half of the
	// sweep is one datum resolve plus one amplified-ground bound per column; the
	// vertical loop over it is nearly free. A camera step that changes only
	// altitude enters a whole new brick layer while entering ZERO new columns,
	// and finds every one of them already here.
	struct FImplicitColumn
	{
		int32 CavernZMm = INT32_MIN;
		int32 LakeZMm = vxc::kNoWaterMm;
		int64 FloodBrickZ = 0;
		int64 GroundUpperMm = MIN_int64;
		int64 GroundFloorMm = MIN_int64;
		bool bCanSkipInterior = false;
		bool bAdmitted = false;
	};
	TMap<FIntPoint, FImplicitColumn> ImplicitColumns;

	// Counters for ONE window step, so the log line can say what the step cost
	// rather than what the box contains.
	int32 ImplicitColumnsSwept = 0;
	int32 ImplicitBricksEvicted = 0;
	int32 ImplicitBricksBuriedSkipped = 0;

	// The far-field river ribbon window currently being filled, or null. Held
	// here rather than on AVoxelRiverRibbonActor because vxc::RiverWetWindow is
	// a voxel-core type and the actor's header is UHT-parsed; the actor drives
	// it through the four Begin/Fill/Finish/Abandon methods on the subsystem.
	// At most one is open at a time -- it is 18 MB at the default 4 km radius.
	std::unique_ptr<vxc::RiverWetWindow> RibbonWindow;
	int32 RibbonBands = 0;
	int32 RibbonPixelMm = 0;

	// Bricks the implicit pass still owes a mesh, drained under a per-tick
	// budget exactly like DirtyBricks.
	TArray<VoxelCoords::FVoxelCoord> PendingImplicitBricks;

	// Drain accounting for the CURRENT implicit refresh -- see the block in
	// RefreshImplicitWater that resets them. These answer "did the water disc
	// finish building before this frame was judged", which nothing else in the
	// engine or in tools/voxel-capture.ps1's settle check can answer.
	uint64 ImplicitRefreshSerial = 0;
	int32 ImplicitCandidatesAtRebuild = 0;
	int32 ImplicitDrainTicks = 0;
	double ImplicitDrainMs = 0.0;
	int32 ImplicitBricksMeshed = 0;
	int32 ImplicitBricksEmpty = 0;
	int32 ImplicitBricksSkippedInterior = 0;
	bool bImplicitDrainReported = false;

	// Reservoir v0's breach-cell set stood here (watershed plan §6.4, work
	// item 8). Retired with the mechanism -- StepFixed's block carries the
	// measurements; FVoxelWaterPerfSnapshot::ReservoirCells is kept and
	// reported as 0 so the HUD/log format does not change shape mid-milestone.

	// Fixed 10Hz accumulator (task item 3).
	float TickAccumSeconds = 0.f;
	static constexpr float FixedStepSeconds = 0.1f; // 10Hz
	// Fixed-timestep spiral-of-death guard: a frame hitch accumulates debt
	// but never demands more than this many catch-up steps in one Tick.
	static constexpr int32 MaxStepsPerFrame = 4;

	// One UWaterChunkComponent per resident vxc::WaterBrick8, keyed by its
	// BrickKey (carried as VoxelCoords::FVoxelCoord -- brick-grid
	// coordinates; see the file-scope ToCoord/ToBrickKey doc comment).
	TMap<VoxelCoords::FVoxelCoord, TObjectPtr<UWaterChunkComponent>> ChunkComponents;

	// --- ADR-0006 water pool (voxel.Water.GPU) ------------------------------
	//
	// The pooled alternative to the two component maps above: a brick's geometry
	// is a RANGE in a shared quad buffer rather than a scene primitive of its
	// own.
	//
	// W5: A HANDFUL OF PRIMITIVES, BUCKETED SPATIALLY, NOT ONE.
	//
	// This used to be one pool component and therefore one primitive, and
	// docs/gpu-water-pool-design.md argued at length why that was sound: the
	// renderer sorts translucent geometry per PRIMITIVE, so pooled water had one
	// sort key for the whole world -- but with a constant base colour, a constant
	// 0.55 opacity and no scene read, two water fragments differed only in their
	// lighting and a stack of N surfaces transmitted (1-0.55)^N in ANY order.
	// The doc also named exactly what would end that: "per-fill shading, foam or
	// caustics", and named the fallback: "per-region sort keys (several
	// primitives, one per spatial bucket)".
	//
	// Depth-tinted absorption and foam land in this same wave, so this is that
	// fallback, built FIRST and deliberately so -- a tint applied over a broken
	// sort makes a sorting artefact and a shading artefact indistinguishable.
	//
	// Each pool is created lazily on the first brick that wants it, on its OWN
	// actor as that actor's ROOT component -- attached as a child of ChunkRoot
	// the primitive never enters the visible set at all
	// (docs/gpu-pool-rendering-notes.md invariant 1).
	struct FPoolBucket
	{
		// Bucket coordinate = floorDiv(brick coordinate, kWaterPoolBucketBricks).
		VoxelCoords::FVoxelCoord Key{0, 0, 0};
		TWeakObjectPtr<UVoxelGpuPoolComponent> Pool;
		// The pool's float32 chunk table cannot hold this world's ~8.4M UU
		// coordinates (invariant 4), so the component carries the offset in its
		// double-precision transform and the table stores brick origins relative
		// to it. DERIVED FROM Key, not from the first brick that happened to
		// arrive: a bucket's rebase is then a pure function of where it is, which
		// is what makes re-homing an emptied bucket (below) a one-line move
		// rather than a migration.
		FVector Rebase = FVector::ZeroVector;
	};
	TArray<FPoolBucket> PoolBuckets;
	TMap<VoxelCoords::FVoxelCoord, int32> PoolBucketIndex;

	// Set while a batched re-mesh loop is running, so a bucket created MID-LOOP
	// can be folded into the same publication scope as the buckets that existed
	// when the loop started. Without it the first tick that reaches a new region
	// would publish once per brick on the new bucket -- the exact per-brick
	// publication tax S1-1 exists to remove, arriving precisely when a burst of
	// bricks is landing.
	struct FVoxelWaterPoolBatch* ActiveBatch = nullptr;

	// Pool handles, keyed exactly like the component maps they replace. Two
	// maps for the same reason there are two component maps: the implicit
	// (worldgen) and CA (simulated) halves own disjoint bricks and hand over
	// between themselves, and MarkMobilizedBricksDirty has to be able to drop
	// one without touching the other.
	//
	// The value is (bucket, handle) rather than a bare handle now: a pool handle
	// is only meaningful against the pool that issued it, and there are several.
	// Storing the bucket beside it -- rather than re-deriving it from the brick
	// coordinate at release time -- is what makes a brick releasable even after
	// its bucket has been re-homed to a different region, which is the one
	// ordering this scheme has to survive.
	struct FPooledBrick
	{
		int32 Bucket = INDEX_NONE;
		int32 Handle = INDEX_NONE;
	};
	TMap<VoxelCoords::FVoxelCoord, FPooledBrick> PoolSlots;
	TMap<VoxelCoords::FVoxelCoord, FPooledBrick> ImplicitPoolSlots;

	// W5 foam input: the bricks vxc::WaterCA reported ACTIVE at the end of the
	// last fixed step. Read at mesh time and written into vertex colour A (via
	// ChunkParams.y on the pooled path, directly on the component path), where
	// M_WaterVoxel turns it into foam.
	//
	// A MEMBER RATHER THAN A LIVE CA QUERY because it is also what drives the
	// SETTLE EDGE: a brick that stops being active is, by definition, no longer
	// marked dirty, so without a record of what WAS active nothing would ever
	// re-mesh it to turn its foam back off. StepFixed diffs this against the new
	// active set and marks the difference dirty exactly once.
	TSet<VoxelCoords::FVoxelCoord> ActiveBricks;

	// 1 Hz status line for the pooled path. Budgeted before it was needed: one
	// primitive drawing thousands of bricks has no per-brick state to inspect,
	// so "what does the pool hold" has to come from a log line or from nowhere.
	float PoolLogAccumSeconds = 0.f;

	// Bricks touched since the last re-mesh pass (task item 4): unioned from
	// vxc::WaterCA::activeBricks() after every fixed step, PLUS any brick an
	// addWater() call (spawn/breach/reservoir top-up) touched directly --
	// the latter needs marking immediately rather than waiting for the next
	// step() to fold it into the CA's own active set, so a fresh pour is
	// visible the same frame it's placed.
	TSet<VoxelCoords::FVoxelCoord> DirtyBricks;

	// --- Perf / HUD bookkeeping (task item 3) -------------------------------
	float PerfRefreshAccumSeconds = 0.f;
	int32 StepsThisWindow = 0;
	int32 LastSteppedBrickCount = 0;
	int32 ReplicatedBytesThisWindow = 0;
	FVoxelWaterPerfSnapshot LastSnapshot;

	// Log-throttle for the voxel.Water.MaxActiveBricks budget warning (task
	// item 3: "log-throttle warning (do not explode)").
	double LastBudgetWarnWorldSeconds = -1000.0;
	// Same, for the §6.5.5 mobilized-ceiling alarm.
	double LastCeilingWarnWorldSeconds = -1000.0;

	// --- Replication v1 (task item 1) ---------------------------------------
	float ReplicationAccumSeconds = 0.f;
	static constexpr float ReplicationIntervalSeconds = 0.2f; // ~5Hz
	static constexpr int32 MaxDiffBytesPerBroadcast = 32 * 1024; // documented cap (task item 1)
	TSet<VoxelCoords::FVoxelCoord> DirtySinceLastBroadcast;

	// Demoted keys awaiting broadcast (work item 9c). A VECTOR, not a TSet, and
	// buffered here rather than pulled from the mobilizer at broadcast time,
	// because `takeRecentlyDemoted()` CLEARS its queue: drain it on the tick
	// that produced it or the removals are lost. Order is preserved because it
	// is the authority's demotion order, which is what the client must replay.
	std::vector<vxc::BrickKey> PendingRemovals;

	// Set from Tick's netmode test, and read by anything that must not run off
	// the authority. Demotion is authority-only doctrine in waterca.h and is
	// deliberately NOT enforced inside voxel-core (which is netmode-free), so
	// this is where that doctrine becomes code.
	bool bAuthority = true;

	// Whether BroadcastWaterDiffs will actually run this session (authority AND
	// networked). Gates the removal QUEUE, not the demotion itself: a standalone
	// world demotes exactly as a server does -- it just has nobody to tell, and
	// queueing removals nobody drains is an unbounded leak in the one
	// configuration most likely to run for hours.
	bool bReplicating = false;

	// --- W4 shallow water, voxel.Water.SWE (ADR-0004) -----------------------
	//
	// BOTH NULL unless the cvar has been armed on a standalone world, and that
	// is the whole safety story for this feature: an unarmed run allocates
	// nothing, queries nothing and branches once per fixed step on a null
	// pointer. swe.h's inertness argument -- "a world in which nothing ever
	// mobilizes is bit-for-bit a world in which this class does not exist" --
	// is inherited verbatim, which is what lets W4 come alive without a version
	// bump or a moved golden.
	//
	// DECLARATION ORDER IS LOAD-BEARING, exactly like Mob-before-CA above:
	// SweCaCoupler holds SweGrid& and WaterCA&, so the grid must outlive the
	// coupler. Members destruct in reverse declaration order, so coupler-last
	// is coupler-first-destroyed.
	//
	// A SINGLE DENSE 128x128 REGION, not a tiled/sparse residency scheme. swe.h
	// says outright that the dense rectangle is the reference core and "a
	// production sheet tiles this; the tick rules do not change" -- so tiling is
	// a later, separable concern that would only obscure this first wiring.
	// 128x128 is also the size ADR-0004 measured end-to-end at 0.364 ms/tick
	// (16,384 columns, linear in columns), i.e. ~3.6% of the 10Hz step's own
	// budget and comfortably inside the subsystem's <2ms/frame target even
	// though it is paid every step rather than every frame.
	std::unique_ptr<vxc::SweGrid> SweSheet;
	std::unique_ptr<vxc::SweCaCoupler> SweCoupler;

	// The conservation ledger for the ENGINE WIRING, which is the part with no
	// tests. swe.h's numerics and the coupler's three transfer channels are
	// golden-pinned and covered by test_swe.cpp's headline invariant; what is
	// entirely new here is the code in this file that decides when to construct
	// a grid, where to put it, which columns to seed, and when to flush it back.
	// That code can lose water in ways voxel-core's tests can never see, so it
	// carries its own runtime check -- see the assert in StepFixed.
	//
	// SweInjected is the running "total injected" of ADR-0004's invariant
	// `ca.totalVolume() + grid.totalVolume() == total injected`. It cannot be a
	// constant here the way it is in a unit test: the live CA is injected into
	// continuously and legitimately from OUTSIDE the coupled window (reservoir
	// top-ups, voxel.SpawnWater, breach seeding, C8 mobilization crediting
	// implicit cavern water, and placements destroying water). So every fixed
	// step folds the change observed OUTSIDE the window into SweInjected, and
	// then requires the window itself -- coupler.step(); grid.step(); ca.step()
	// -- to move the sum by exactly zero. That is the same claim, checked once
	// per tick instead of once per run, and it localises any failure to the
	// three calls that could have caused it.
	int64_t SweInjected = 0;
	int64_t SweLastCoupledTotal = 0;
	int64_t SweConservationFailures = 0;

	// Per-column depth as of the last tick, row-major over the sheet, for
	// dirty-marking the render bricks above a column whose depth moved.
	//
	// WHY THIS IS NEEDED AT ALL. The CA drives re-meshing through its own
	// changed-brick set, and sheet water is not in the CA -- once a column
	// promotes, its depth can change every tick while the CA reports nothing
	// dirty at all, so the surface would simply freeze at whatever it looked
	// like when the water left the CA. Diffing 16,384 ints is negligible next
	// to the 0.364 ms/tick the sheet step itself costs.
	std::vector<int32_t> SweLastDepth;

	// Sheet COLUMN INDICES (cx + sizeX*cy, the coupler's own scan order) whose
	// seated bed a terrain edit has invalidated, waiting to be re-seated by
	// ReseatEditedSweBeds in the coupled window. See that function for why the
	// queue exists at all; see NotifyTerrainRegionEdited for who fills it.
	//
	// A SET, drained in sorted index order, so the pass is deterministic
	// regardless of the order edits arrive in. Bounded by the sheet itself
	// (16,384 entries), because an index can only be present once and a column
	// leaves the queue for good the first tick it is CA-owned.
	//
	// Indices are meaningful only against the grid origin they were computed
	// from, so MaybeArmSwe clears this on every arm and disarm.
	TSet<int32> SwePendingBedReseat;
	int64 SweBedsReseated = 0;

	// One-shot latches so a refused arm request (wrong net mode, nothing to
	// centre on) says so once rather than once per frame for the whole session.
	bool bSweRefusalLogged = false;
	double SweLastStatusWorldSeconds = -1000.0;

	// --- W3 rivers (voxel.Water.Rivers, plan S3.7 Layer R) ------------------
	//
	// Null unless armed, exactly like the SWE pair above and for exactly the
	// same reason: an unarmed run allocates nothing and branches once per fixed
	// step on a null pointer, so rivercouple.h's own "a total no-op when
	// disabled" claim survives all the way out to the engine.
	//
	// DECLARATION ORDER IS LOAD-BEARING a third time: RiverCaCoupler holds
	// RiverNetwork&, WaterCA& and ITileSampler&, so the network must outlive the
	// coupler and both must outlive nothing else here. Coupler last == coupler
	// destroyed first.
	std::unique_ptr<vxc::RiverNetwork> Rivers;
	std::unique_ptr<vxc::RiverCaCoupler> RiverCoupler;

	// Per-segment baseflow injected every river tick, captured at BUILD time.
	//
	// It has to be captured, not read live: RiverSegment::discharge means the
	// build-time catchment estimate before the first step() and the routed
	// outflow after it (rivernet.h says so explicitly), so reading it live
	// after the first tick would make the rain that falls on a catchment depend
	// on how much water happens to be in the reach -- a feedback loop, not a
	// climate.
	//
	// Scaled DOWN from the raw catchment accumulation by kRiverBaseflowShift.
	// The raw value is a sum of mm/yr over every upstream pixel and reaches
	// tens of millions on a trunk reach; injecting that unscaled would (a)
	// overflow RiverSegment's int32 storage inside an hour behind a dam and
	// (b) ask the coupler for orders of magnitude more fill than
	// maxFillPerSegmentPerTick will ever hand over, so the surplus would just
	// pile up unspendably. The shift puts a trunk reach at roughly the coupler's
	// own per-tick ceiling, which is the only rate that can actually be
	// consumed.
	std::vector<int32_t> RiverBaseflow;

	// ~1 Hz cadence (plan S3.7: "segment graph ticks ~1Hz server-side"), driven
	// off world time inside StepFixed rather than off a second accumulator in
	// Tick -- the same idiom SweLastStatusWorldSeconds already uses, and it puts
	// the river's CA injection inside the fixed step where the SWE conservation
	// ledger can see it as an external injection rather than as a leak.
	static constexpr double RiverStepSeconds = 1.0;
	double RiverLastTickWorldSeconds = -1000.0;
	double RiverLastStatusWorldSeconds = -1000.0;
	int64 RiverPromotions = 0;
	bool bRiverRefusalLogged = false;
};

namespace
{
// Marks every water brick spanning [VzStart, VzStart + ceil(PlacedAmount/255)]
// at (Vx,Vy) dirty for re-mesh AND replication -- addWater() stacks straight
// up a column, so this is the full set of bricks a single addWater() call
// could possibly have touched (a one-brick apron above the theoretical span
// costs nothing: findBrick() returning null there is a harmless no-op in
// RemeshDirtyBricks/SerializeWaterDiffs).
void MarkColumnDirty(FVoxelWaterImpl& Impl, int64_t Vx, int64_t Vy, int64_t VzStart, uint32_t PlacedAmount)
{
	const int64_t CellsSpan = int64_t(PlacedAmount) / 255 + 2;
	for (int64_t Dz = 0; Dz <= CellsSpan; ++Dz)
	{
		const VoxelCoords::FVoxelCoord C = ToCoord(vxc::waterKeyForVoxel(Vx, Vy, VzStart + Dz));
		Impl.DirtyBricks.Add(C);
		Impl.DirtySinceLastBroadcast.Add(C);
	}
}
} // namespace

// ADR-0005 water persistence: forward declarations. The definitions live in
// the big anonymous namespace below (they lean on MarkMobilizedBricksDirty /
// ToCoord, defined there), but OnWorldBeginPlay / Deinitialize / SaveWaterState
// above them need to call them.
namespace
{
FString GetWaterSaveFilePath(uint64 Seed);
bool SaveWaterStateToDisk(const FVoxelWaterImpl& Impl, uint64 Seed);
void LoadWaterStateFromDisk(FVoxelWaterImpl& Impl, uint64 Seed);

// Water re-architecture Phase 2, same forward-declaration reason: defined below
// beside the persistence code, called from OnWorldBeginPlay above.
void EnsureBasinLedger(FVoxelWaterImpl& Impl);
bool SaveHydroStateToDisk(const FVoxelWaterImpl& Impl, uint64 Seed);
void LoadHydroStateFromDisk(FVoxelWaterImpl& Impl, uint64 Seed);

// W4 (ADR-0004). Same forward-declaration reason as the three above: these are
// defined down beside StepFixed (they lean on MarkColumnDirty and on the
// meshing helpers' brick bookkeeping), but Tick() and SaveWaterState() are
// above them and both have to call them.
void MaybeArmSwe(FVoxelWaterImpl& Impl, UWorld* World);
void FlushSweIntoCA(FVoxelWaterImpl& Impl, const TCHAR* Reason);
void MarkSweDepthChangesDirty(FVoxelWaterImpl& Impl);

// voxel.Water.ImplicitOcean (watershed plan §6.4). Re-reads the cvar on a step
// boundary and REFUSES the change, loudly, unless the world is in the one state
// where flipping it is safe.
//
// WHY A REFUSAL AND NOT JUST A LATCH ASSIGNMENT. The implicit field is what
// makes unmobilized water a wall (voxelcore/waterca.h's ownership partition),
// and mobilization is a one-way surrender that is SAVED. Turning the ocean ON
// under a CA that already holds water below the datum would make the implicit
// field claim 255 units in cells the CA already owns; the next mobilization of
// that brick credits them a second time and the ledger reports a shortfall --
// a number nobody watches, in a system whose only alarm is that number.
// Turning it OFF under a mobilized set does not corrupt anything but does
// leave already-mobilized sea sitting there, so the "control" frame would not
// be a control. Both cases are refused with a line that says which.
void MaybeRelatchImplicitOcean(FVoxelWaterImpl& Impl);
} // namespace

// UVoxelWaterSubsystem ------------------------------------------------------

UVoxelWaterSubsystem::UVoxelWaterSubsystem() = default;
UVoxelWaterSubsystem::~UVoxelWaterSubsystem() = default;
UVoxelWaterSubsystem::UVoxelWaterSubsystem(FVTableHelper& Helper)
	: Super(Helper)
{
}

void UVoxelWaterSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Direct dependency on the terrain subsystem (task spec item 1): both are
	// UWorldSubsystem-derived, so InitializeDependency handles ordering --
	// UVoxelWorldSubsystem::Impl is guaranteed constructed by the time this
	// call returns, so IsSolidAtVoxel is safe to bind into the CA's SolidFn
	// right away (it isn't actually CALLED until streaming/breach/spawn
	// touches a voxel, well after both subsystems' Initialize has run).
	UVoxelWorldSubsystem* Terrain = Collection.InitializeDependency<UVoxelWorldSubsystem>();
	if (!Terrain)
	{
		UE_LOG(LogVoxelWater, Error,
		       TEXT("UVoxelWaterSubsystem::Initialize: no UVoxelWorldSubsystem -- water CA cannot bridge to terrain ")
		       TEXT("solidity; water is disabled for this run."));
		return;
	}

	Impl = MakeUnique<FVoxelWaterImpl>(*Terrain);

	// DEBUG WATER MARKER (-VoxelWaterMarker=1, -VoxelWaterMarkerOcean=0).
	//
	// Installed HERE and nowhere else, for three reasons that are all lifetime
	// or threading rather than taste:
	//
	//  * this subsystem already owns the composed lake+river sampler, so the
	//    marker reuses it instead of standing up a second FineTileSampler over
	//    the same tiles;
	//  * Initialize ran InitializeDependency<UVoxelWorldSubsystem> above, so the
	//    terrain world exists, and both Initializes are well before any mesher
	//    job -- which matters because the brick caches are session-lifetime and
	//    a later install would leave unmarked bricks resident beside marked ones;
	//  * the sampler must be reachable from worker threads, which `Water` is
	//    not. LockedWaterSampler is what makes it so.
	// Phase 2: stand the basin ledger up and bind it to the sampler, so from
	// here on every consumer of a lake's height reads the LEDGER'S level rather
	// than the wire's. With no fine tier this is a no-op and the world behaves
	// exactly as it did before.
	EnsureBasinLedger(*Impl);

	int32 MarkerOn = 0;
	const bool bMarker = FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterMarker="), MarkerOn)
		                     ? MarkerOn != 0
		                     : FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterMarker"));
	if (bMarker)
	{
		int32 OceanOn = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterMarkerOcean="), OceanOn);
		Impl->MarkerWater = std::make_unique<vxc::LockedWaterSampler>(*Impl->Water);
		// ANNOUNCE ONLY WHAT ACTUALLY HAPPENED. InstallWaterMarker refuses when
		// the GPU mesh fork is on, and this line used to print regardless -- so
		// a refused run still said "water is drawn as SOLID magenta", which is
		// the precise shape of misleading evidence the refusal exists to
		// prevent. The terrain side has already logged the reason at Error.
		if (Terrain->InstallWaterMarker(Impl->MarkerWater.get(), OceanOn != 0))
		{
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("VoxelWaterMarker: ON (ocean %s). Water is drawn as SOLID magenta MAT_WATERMARK voxels ")
			       TEXT("through the TERRAIN path, so it is visible at full clipmap range rather than only inside ")
			       TEXT("the +/-25.6 m near-field disc. This is a debug instrument."),
			       OceanOn != 0 ? TEXT("INCLUDED") : TEXT("excluded"));
		}
		else
		{
			// Drop the sampler we just built: nothing borrows it now, and
			// leaving it live invites a later reader into thinking it is
			// installed.
			Impl->MarkerWater.reset();
		}
	}
}

void UVoxelWaterSubsystem::Deinitialize()
{
	// THE MARKER IS A BORROWED POINTER INTO Impl->MarkerWater. Terrain may
	// outlive water, so clear it before this subsystem's impl goes away or the
	// amplifier is left dereferencing freed memory on the next mesher job.
	if (Impl && Impl->MarkerWater)
	{
		if (UVoxelWorldSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UVoxelWorldSubsystem>() : nullptr)
		{
			Terrain->InstallWaterMarker(nullptr);
		}
	}
	// ADR-0005: autosave the water blob on shutdown, mirroring
	// UVoxelWorldSubsystem::Deinitialize's edit-log autosave lifetime exactly --
	// authority only, and only for a world that genuinely began play (see
	// bWorldBegunPlay). Additionally gated on there being real water state to
	// persist (stored or mobilized bricks) so a run that never disturbed
	// underground water leaves no sibling .vxwater to shadow a later same-seed
	// session -- the edit log has no such gate because every world has a log,
	// but an all-implicit world has nothing to serialize and an empty blob would
	// only add a spurious file.
	if (Impl && bWorldBegunPlay)
	{
		UWorld* World = GetWorld();
		const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
		// W4 (ADR-0004): the sheet counts as "real water state to persist" too,
		// and it has to be named explicitly here. Promotion MOVES units out of
		// CA cells, so a session whose whole pour has been promoted can have
		// storedBrickCount() == 0 with a full sheet -- and the pre-W4 gate would
		// then skip the autosave, drop the grid with Impl.Reset() below, and
		// lose the lot on shutdown. SaveWaterState flushes the sheet back into
		// the CA before serialising, so once we decide to save, the blob is
		// complete.
		const bool bHaveSheetWater = Impl->SweSheet && Impl->SweSheet->totalVolume() > 0;
		if (NetMode != NM_Client &&
		    (Impl->CA.storedBrickCount() > 0 || !Impl->Mob.mobilizedBricks().empty() || bHaveSheetWater))
		{
			SaveWaterState();
		}
	}

	ChunkRoot = nullptr;
	ChunkOwner = nullptr;
	WaterMaterial = nullptr;
	Impl.Reset();

	Super::Deinitialize();
}

bool UVoxelWaterSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same scope as UVoxelWorldSubsystem: game worlds only.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

void UVoxelWaterSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (!InWorld.IsGameWorld() || !Impl)
	{
		return;
	}

	// ADR-0005: from here on this is a genuine gameplay world (same reasoning as
	// UVoxelWorldSubsystem's bWorldBegunPlay) -- gates Deinitialize's autosave.
	bWorldBegunPlay = true;

	// ADR-0005 water persistence: the authority (server/listen/standalone) loads
	// Saved/VoxelWorlds/<seed>.vxwater, if present, before the first fixed step
	// ever runs (Tick() below). This is the earliest point OnWorldBeginPlay
	// reaches after the game-world/Impl guard, and it runs BEFORE the
	// dedicated-server early-return below so it applies to every authority role,
	// exactly mirroring UVoxelWorldSubsystem's LoadEditLogFromDisk placement. The
	// live CA/mob are still fresh here (no addWater/mobilize has run yet), which
	// is vxc::WaterState::applyTo's precondition. Fills-first load, three
	// failure modes handled loudly -- see LoadWaterStateFromDisk. NM_Client never
	// loads its own file: a joining client mirrors state via replication instead.
	if (InWorld.GetNetMode() != NM_Client)
	{
		LoadWaterStateFromDisk(*Impl, Impl->Terrain.GetSeed());
		// Phase 2's blob, from its OWN file and with its OWN failure handling --
		// a separate call rather than a tail of the one above because
		// LoadWaterStateFromDisk returns early on all three of its failure modes,
		// and "the CA blob was missing" must not silently also mean "every lake
		// forgot where it stood". Same NM_Client rule: a joining client mirrors
		// the scalars through the diff channel, it does not load them.
		LoadHydroStateFromDisk(*Impl, Impl->Terrain.GetSeed());
	}

	// Dedicated server: no viewport, so no render chunks -- but the CA still
	// ticks authoritatively regardless (Tick() below doesn't check
	// ChunkOwner for the simulation half, only for the re-mesh half). Same
	// reasoning as UVoxelWorldSubsystem's identical dedicated-server branch.
	if (InWorld.GetNetMode() == NM_DedicatedServer)
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("Water rendering DISABLED (NM_DedicatedServer has no viewport): the pressure CA still ticks ")
		       TEXT("authoritatively at %.0fHz."),
		       1.f / FVoxelWaterImpl::FixedStepSeconds);
		return;
	}

	WaterMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_WaterVoxel.M_WaterVoxel")));
	if (WaterMaterial == nullptr)
	{
		WaterMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		UE_LOG(LogVoxelWater, Warning, TEXT("M_WaterVoxel not found at /Game/Voxel/M_WaterVoxel -- using engine default material."));
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ChunkOwner = InWorld.SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (ChunkOwner == nullptr)
	{
		UE_LOG(LogVoxelWater, Error, TEXT("Failed to spawn the water chunk owner actor; water rendering will not start."));
		return;
	}

	ChunkRoot = NewObject<USceneComponent>(ChunkOwner, TEXT("VoxelWaterChunkRoot"));
	ChunkOwner->SetRootComponent(ChunkRoot);
	ChunkRoot->RegisterComponent();
#if WITH_EDITOR
	ChunkOwner->SetActorLabel(TEXT("VoxelWaterChunkOwner"));
#endif

	UE_LOG(LogVoxelWater, Log, TEXT("Water subsystem initialized: fixed step %.0fHz, MaxActiveBricks budget=%d"),
	       1.f / FVoxelWaterImpl::FixedStepSeconds, VoxelDebug::GetWaterMaxActiveBricks());
}

namespace
{
// --- ADR-0006 water pool (voxel.Water.GPU) ---------------------------------
//
// WHY A SECOND INSTANCE OF UVoxelGpuPoolComponent RATHER THAN A WATER-SPECIFIC
// COPY. The pool is generic in everything water needed it to be:
//
//   * The quad packing is shared already. PackVoxelChunkQuad's fields are all
//     <= 8 bits and a water brick is 8 voxels on an edge, so brick-local
//     coordinates fit the same encoding terrain's 32-voxel chunk-local ones do
//     -- with 2 bits to spare rather than 3. No second decode path.
//   * The vertex factory writes exactly the one channel M_WaterVoxel reads.
//     That material's only input from the geometry is VertexColor.G (AO), and
//     VoxelQuadVertexFactory.ush writes AO into .G. Channels R/B/A carry
//     terrain-specific biome-tint and climate values, which this material does
//     not sample -- so they are inert here, not wrong.
//   * Translucency is a MATERIAL property, and the proxy already derives its
//     view relevance from FMaterialRelevance. Nothing in the draw path had to
//     learn about blend modes.
//
// Three things did have to be parameterised, and all three are one-line
// setters on the shared component rather than forks of it: the log prefix
// (two pools printing the same line is worse than no line), the per-entry edge
// length used to grow bounds (8 voxels, not 32), and the chunk-table floor.
//
// The one genuine BEHAVIOURAL fix the water instance forced is in the pool
// itself, not here: chunk-table entries were never recycled. See
// UVoxelGpuPoolComponent::FreeChunkIds.
//
// --- W5: SEVERAL INSTANCES, BUCKETED BY WORLD SPACE ------------------------
//
// WHY THE COUNT CHANGED. UE sorts translucent draw commands by a key whose
// distance field is filled from `PrimitiveBounds[PrimitiveIndex]
// .BoxSphereBounds.Origin` (UpdateTranslucentMeshSortKeys, MeshDrawCommands.cpp)
// -- the PRIMITIVE's bounds centre, one value for every command the primitive
// emits. Within a primitive the remaining tie-break is the 16-bit
// MeshIdInPrimitive, which is a STABLE id and not a per-view depth order. So a
// single water primitive genuinely cannot sort its own contents against itself
// or against anything else at better than whole-pool granularity, however many
// mesh batches it emits. Splitting the primitive is the only lever the renderer
// offers short of an OIT path.
//
// WHY 64 BRICKS (51.2 m) ON A SIDE. Sized from the largest water body this
// system actually produces, which is the implicit cavern-lake shell:
// RefreshImplicitWater sweeps a 65 x 65 x 33 BRICK disc, i.e. 52 x 52 x 26 m.
// A bucket larger than that disc gives back nothing at all -- the whole lake
// would land in one bucket and we would be exactly where we started. 64 makes
// the disc span 2 x 2 buckets horizontally and 1-2 vertically, so a typical
// scene draws 4-8 water primitives instead of 1. That is the "handful" this is
// aiming at, and it is three orders of magnitude below the per-brick component
// path's 2,231 primitives at the same anchor (docs/gpu-water-pool-design.md).
//
// REJECTED, AND WHY:
//
//   * 16 bricks (12.8 m). Sorts better, and the implicit disc alone would then
//     want up to 5x5x3 = 75 primitives. Not fatal -- still far below the
//     component path -- but it re-opens the FScene::AddPrimitive funnel ADR-0006
//     exists to close, in exchange for a granularity that STILL does not fix the
//     dominant artefact (see the next point). Bad trade.
//   * Trying to fix the artefact properly with buckets at all. Water surfaces
//     stack most often at ONE-BRICK range: the near side wall, the surface and
//     the far side wall of a single stepped pool. No bucket size above a brick
//     orders those, and a bucket per brick is the per-brick component path with
//     extra steps. What bucketing DOES fix is long-range compositing -- one lake
//     seen through another, water above a cavern lake, and water against a
//     foreign translucent primitive -- and that is the honest claim.
//   * Bucketing only in XY, with Z unbounded. Cheaper (fewer buckets), and
//     wrong for the case this project has most of: a surface lake directly above
//     a cavern lake is the textbook water-over-water stack, and a Z-unbounded
//     column puts both in one bucket.
//   * Keeping one component and ranking its FMeshBatches with MeshIdInPrimitive
//     per view. It would order water against water for free, with no extra
//     primitive -- but MeshIdInPrimitive sits BELOW Distance in the sort key, so
//     it can only break ties WITHIN one primitive. Foreign translucent geometry
//     would still sort against the whole pool as one unit, which is the third
//     break condition gpu-water-pool-design.md names. Kept in reserve as an
//     intra-bucket refinement.
//
// WHAT A BUCKET COSTS. Its own quad buffer (kWaterBucketCapacityQuads, 4 MB),
// its own chunk table, its own proxy and its own cull walk. That is why buckets
// are RECYCLED rather than accumulated: water follows the camera, so buckets
// empty out behind it, and an emptied bucket is re-homed to a new region
// instead of allocating a new one. The rebase is a pure function of the bucket
// key, so re-homing is a SetWorldLocation and nothing else.

// Edge length of one sort bucket, in water bricks. See the essay above.
constexpr int32 kWaterPoolBucketBricks = 64;

// Ceiling on live bucket primitives. Not a tuning knob: past this, bricks are
// routed to their NEAREST live bucket, which draws them in the right place with
// the wrong sort key -- i.e. it degrades to exactly the pre-W5 behaviour for
// the overflow rather than dropping geometry. 12 is ~1.5x the 4-8 a typical
// scene wants, so reaching it at all means something unusual is on screen.
constexpr int32 kMaxWaterPoolBuckets = 12;

// Per-bucket quad capacity. 4 MB each; 12 buckets is a 48 MB ceiling against
// the single pool's 32 MB, and the typical 4-8 live buckets sit at 16-32 MB,
// i.e. at or below where this started.
//
// SIZED AGAINST THE WHOLE WORLD'S WATER, NOT A BUCKET'S SHARE, deliberately.
// The measured peak is 28,862 quads across 2,231 implicit bricks (~13 quads per
// brick). Even if EVERY implicit brick and the CA's entire 4,096-brick budget
// landed in ONE bucket at a pessimistic 80 quads each, that is ~500k quads. A
// per-bucket pool cannot borrow from its neighbours the way one big pool could,
// so the headroom has to cover the degenerate distribution rather than the
// expected one.
constexpr uint32 kWaterBucketCapacityQuads = 512u * 1024u;

// Per-bucket chunk-table floor. Crossing it is not an error but it IS a full
// render-state rebuild and a full quad re-upload, so it wants to sit above the
// bucket's steady-state entry count rather than near it. 8,192 is ~3.7x the
// 2,231 implicit bricks measured for a whole scene.
constexpr int32 kWaterBucketTableCapacity = 8192;

// Which bucket a brick belongs to. floorDiv, not integer division: brick
// coordinates go negative and C++ truncates toward zero, which would fold
// bricks at -1 and +1 into the same bucket and put a sort-bucket seam through
// the world origin.
VoxelCoords::FVoxelCoord WaterPoolBucketKey(const VoxelCoords::FVoxelCoord& BrickCoord)
{
	return VoxelCoords::FVoxelCoord{
		int64(vxc::floorDiv(int64_t(BrickCoord.X), int64_t(kWaterPoolBucketBricks))),
		int64(vxc::floorDiv(int64_t(BrickCoord.Y), int64_t(kWaterPoolBucketBricks))),
		int64(vxc::floorDiv(int64_t(BrickCoord.Z), int64_t(kWaterPoolBucketBricks)))};
}

// A bucket's rebase: the world-space corner of the bucket itself. Derived, not
// remembered -- see FPoolBucket::Rebase.
FVector WaterPoolBucketRebase(const VoxelCoords::FVoxelCoord& BucketKey)
{
	constexpr double kBucketUU =
		double(kWaterPoolBucketBricks) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
	return FVector(double(BucketKey.X) * kBucketUU, double(BucketKey.Y) * kBucketUU,
	               double(BucketKey.Z) * kBucketUU);
}

UVoxelGpuPoolComponent* SpawnWaterPoolPrimitive(AActor* ChunkOwner, UMaterialInterface* Material,
                                                const VoxelCoords::FVoxelCoord& BucketKey)
{
	UWorld* World = ChunkOwner ? ChunkOwner->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return nullptr;
	}

	// ITS OWN ACTOR, WITH THE POOL AS ROOT. Not attached under ChunkRoot beside
	// the per-brick components: as a child the primitive never enters the
	// visible set -- GetDynamicMeshElements is never called, with a live proxy,
	// valid bounds and no warning anywhere (gpu-pool-rendering-notes.md
	// invariant 1). The terrain pool learned this the expensive way; there was
	// no reason to learn it twice.
	FActorSpawnParameters PoolSpawnParams;
	PoolSpawnParams.ObjectFlags |= RF_Transient;
	PoolSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* PoolOwner = World->SpawnActor<AActor>(
		AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, PoolSpawnParams);
	if (PoolOwner == nullptr)
	{
		return nullptr;
	}
#if WITH_EDITOR
	PoolOwner->SetActorLabel(FString::Printf(TEXT("VoxelWaterPoolOwner_%lld_%lld_%lld"), (long long)BucketKey.X,
	                                         (long long)BucketKey.Y, (long long)BucketKey.Z));
#endif

	UVoxelGpuPoolComponent* Pool = NewObject<UVoxelGpuPoolComponent>(PoolOwner);
	// NAMED PER BUCKET. Every pool diagnostic in this project is a log line
	// keyed on the pool name (the pool has no per-brick state to inspect), and
	// several buckets all printing "VoxelWaterPool draw SUBMITTED" would make
	// the one number that matters unattributable -- which is the same reason
	// the water pool got a name distinct from terrain's in the first place.
	Pool->SetPoolName(FString::Printf(TEXT("VoxelWaterPool[%lld,%lld,%lld]"), (long long)BucketKey.X,
	                                  (long long)BucketKey.Y, (long long)BucketKey.Z));
	// vxc::WaterBrick8::kEdge. Terrain's 32 would still be CORRECT here (a
	// larger bounds box is a looser cull, never a lost draw), but 8 is what the
	// geometry actually spans and this pool's bricks are 1/64th the volume.
	Pool->SetChunkEdgeVoxels(vxc::WaterBrick8::kEdge);
	// Above this BUCKET's plausible resident-brick count, so an ordinary camera
	// move does not oscillate across the table capacity and pay a full
	// render-state rebuild -- and a full re-upload of the quad buffer -- each
	// time it does. See kWaterBucketTableCapacity.
	Pool->SetChunkTableCapacity(kWaterBucketTableCapacity);
	// Makes the pooled path reproduce water's vertex-colour convention rather
	// than terrain's: R = the CA fill fraction this subsystem's meshing sampler
	// puts in the quad's `mat` byte (terrain puts a binary sky-facing biome
	// flag there), B = the per-vertex top-boundary flag (terrain puts per-chunk
	// climate there). M_WaterVoxel's World Position Offset consumes both to
	// lower a partial cell's surface to its fill height.
	//
	// WITHOUT this the pooled path would hand the material terrain's biome flag
	// and climate, drawing every water surface at full height -- silently
	// agreeing with the old >=128 behaviour while the component path, which is
	// the default, showed the stepped surface. Must precede proxy creation
	// (RegisterComponent below); the proxy takes a copy.
	Pool->SetWaterMode(true);
	// M_WaterVoxel. Without it the proxy silently falls back to the engine
	// default material, which is opaque -- water would render as grey boxes and
	// look like a geometry bug rather than a material one.
	Pool->SetChunkMaterial(Material);
	PoolOwner->SetRootComponent(Pool);
	Pool->RegisterComponent();

	// SetWorldLocation AFTER RegisterComponent, never SetRelativeLocation
	// before: SetRootComponent on a freshly NewObject'd component installs an
	// identity transform and redefines the actor's location as the world
	// origin, 84 km from anything worth drawing (invariant 2).
	//
	// THE REBASE IS THE BUCKET'S OWN CORNER, not the first brick that arrived.
	// That is what makes it derivable rather than remembered, and therefore what
	// makes an emptied bucket re-homable by moving the component and nothing
	// else. It also bounds every chunk-table origin to [0, 5120) UU on each
	// axis -- a bucket edge -- which is a far tighter float32 range than the
	// old first-brick rebase gave, since that one could be anywhere in the world
	// relative to the water that later joined it.
	Pool->SetWorldLocation(WaterPoolBucketRebase(BucketKey));

	Pool->InitPool(kWaterBucketCapacityQuads);
	return Pool;
}

UVoxelGpuPoolComponent* GetBucketPool(FVoxelWaterImpl& Impl, int32 BucketIndex)
{
	return Impl.PoolBuckets.IsValidIndex(BucketIndex) ? Impl.PoolBuckets[BucketIndex].Pool.Get() : nullptr;
}

} // namespace

// AT GLOBAL SCOPE, NOT IN THE ANONYMOUS NAMESPACE ABOVE, and that is forced
// rather than stylistic: FVoxelWaterImpl declares `struct FVoxelWaterPoolBatch*
// ActiveBatch`, and an elaborated type specifier inside a class declares the
// name in the nearest ENCLOSING namespace -- the global one, since
// FVoxelWaterImpl is a global-scope struct. Defining it in the anonymous
// namespace instead would create a second, unrelated type with the same
// spelling, and the assignment in the constructor would not compile.
//
// Opens one publication scope per live bucket, and keeps accepting new buckets
// for as long as it is alive. See FVoxelWaterImpl::ActiveBatch for why the
// second half matters.
//
// The scopes are held by pointer in an array purely because
// UVoxelGpuPoolComponent::FScopedBatch is deliberately non-copyable and
// non-movable (its depth counter is the whole mechanism), so it cannot live in
// a TArray by value.
struct FVoxelWaterPoolBatch
{
	explicit FVoxelWaterPoolBatch(FVoxelWaterImpl& InImpl)
		: Impl(InImpl)
	{
		for (const FVoxelWaterImpl::FPoolBucket& Bucket : Impl.PoolBuckets)
		{
			if (UVoxelGpuPoolComponent* Pool = Bucket.Pool.Get())
			{
				Scopes.Emplace(MakeUnique<UVoxelGpuPoolComponent::FScopedBatch>(Pool));
			}
		}
		// LAST, so a bucket created by anything above cannot be adopted twice.
		Impl.ActiveBatch = this;
	}

	~FVoxelWaterPoolBatch()
	{
		// Cleared BEFORE the scopes close: ~FScopedBatch flushes, a flush can in
		// principle reach code that creates geometry, and adopting a bucket into
		// a batch that is already unwinding would open a scope nothing closes.
		Impl.ActiveBatch = nullptr;
		Scopes.Empty();
	}

	FVoxelWaterPoolBatch(const FVoxelWaterPoolBatch&) = delete;
	FVoxelWaterPoolBatch& operator=(const FVoxelWaterPoolBatch&) = delete;

	void Adopt(UVoxelGpuPoolComponent* Pool)
	{
		if (Pool != nullptr)
		{
			Scopes.Emplace(MakeUnique<UVoxelGpuPoolComponent::FScopedBatch>(Pool));
		}
	}

	FVoxelWaterImpl& Impl;
	TArray<TUniquePtr<UVoxelGpuPoolComponent::FScopedBatch>> Scopes;
};

namespace
{

// Routes a brick to its sort bucket, creating or re-homing a pool primitive if
// it needs one. Returns INDEX_NONE only if no primitive could be produced at
// all, which the caller treats exactly like a full pool: the brick goes
// undrawn rather than half-drawn.
int32 GetOrCreateWaterPoolBucket(FVoxelWaterImpl& Impl, AActor* ChunkOwner, UMaterialInterface* Material,
                                 const VoxelCoords::FVoxelCoord& BrickCoord)
{
	const VoxelCoords::FVoxelCoord BucketKey = WaterPoolBucketKey(BrickCoord);
	if (const int32* Existing = Impl.PoolBucketIndex.Find(BucketKey))
	{
		if (GetBucketPool(Impl, *Existing) != nullptr)
		{
			return *Existing;
		}
		// The component was garbage collected out from under us (world teardown
		// races, an editor PIE stop). Drop the mapping and fall through to make a
		// new one rather than handing back an index whose pool is gone.
		Impl.PoolBucketIndex.Remove(BucketKey);
	}

	// RE-HOME AN EMPTY BUCKET BEFORE ALLOCATING A NEW ONE. Water follows the
	// camera; buckets behind it drain to zero live entries and would otherwise
	// sit there holding 4 MB and a scene primitive for the rest of the session.
	// An empty pool has no chunk-table entry, no run and no live quad naming its
	// old rebase, so moving it is a transform change and nothing else.
	for (int32 I = 0; I < Impl.PoolBuckets.Num(); ++I)
	{
		UVoxelGpuPoolComponent* Pool = Impl.PoolBuckets[I].Pool.Get();
		if (Pool == nullptr || Pool->GetNumChunks() != 0)
		{
			continue;
		}
		Impl.PoolBucketIndex.Remove(Impl.PoolBuckets[I].Key);
		Impl.PoolBuckets[I].Key = BucketKey;
		Impl.PoolBuckets[I].Rebase = WaterPoolBucketRebase(BucketKey);
		Pool->SetWorldLocation(Impl.PoolBuckets[I].Rebase);
		Impl.PoolBucketIndex.Add(BucketKey, I);
		UE_LOG(LogVoxelWater, Verbose,
		       TEXT("voxel.Water.GPU: re-homed empty water bucket %d to (%lld,%lld,%lld)"),
		       I, (long long)BucketKey.X, (long long)BucketKey.Y, (long long)BucketKey.Z);
		return I;
	}

	if (Impl.PoolBuckets.Num() < kMaxWaterPoolBuckets)
	{
		UVoxelGpuPoolComponent* Pool = SpawnWaterPoolPrimitive(ChunkOwner, Material, BucketKey);
		if (Pool == nullptr)
		{
			return INDEX_NONE;
		}
		FVoxelWaterImpl::FPoolBucket Bucket;
		Bucket.Key = BucketKey;
		Bucket.Pool = Pool;
		Bucket.Rebase = WaterPoolBucketRebase(BucketKey);
		const int32 Index = Impl.PoolBuckets.Add(Bucket);
		Impl.PoolBucketIndex.Add(BucketKey, Index);
		// A bucket born mid-loop joins the loop's publication scope. Without
		// this, the first tick that reaches a new region publishes once per
		// brick on it -- see FVoxelWaterImpl::ActiveBatch.
		if (Impl.ActiveBatch != nullptr)
		{
			Impl.ActiveBatch->Adopt(Pool);
		}
		UE_LOG(LogVoxelWater, Log,
		       TEXT("voxel.Water.GPU: water bucket %d up at (%lld,%lld,%lld), %u quad capacity (%.1f MB), "
		            "rebase (%.0f,%.0f,%.0f). %d/%d buckets live."),
		       Index, (long long)BucketKey.X, (long long)BucketKey.Y, (long long)BucketKey.Z,
		       kWaterBucketCapacityQuads, double(kWaterBucketCapacityQuads) * 8.0 / (1024.0 * 1024.0),
		       Bucket.Rebase.X, Bucket.Rebase.Y, Bucket.Rebase.Z, Impl.PoolBuckets.Num(), kMaxWaterPoolBuckets);
		return Index;
	}

	// AT THE CAP. Fall back to the NEAREST live bucket rather than dropping the
	// brick. It draws in the right place with the wrong sort key, which is
	// precisely the pre-W5 behaviour for that brick and no worse.
	//
	// The chunk-table origin is then relative to a FOREIGN bucket's rebase, so
	// this does spend float32 precision it would not otherwise: nearest-bucket
	// keeps that offset as small as the live set allows, but after a teleport it
	// could be kilometres, where float32 resolves to ~centimetres rather than the
	// sub-millimetre a same-bucket brick enjoys. That is a visible-at-nothing
	// error on 10 cm voxels and is accepted; it is also self-correcting, since
	// the buckets behind the camera drain and the next brick re-homes one.
	int32 Best = INDEX_NONE;
	int64 BestDistSq = MAX_int64;
	for (int32 I = 0; I < Impl.PoolBuckets.Num(); ++I)
	{
		if (Impl.PoolBuckets[I].Pool.Get() == nullptr)
		{
			continue;
		}
		const int64 Dx = int64(Impl.PoolBuckets[I].Key.X) - int64(BucketKey.X);
		const int64 Dy = int64(Impl.PoolBuckets[I].Key.Y) - int64(BucketKey.Y);
		const int64 Dz = int64(Impl.PoolBuckets[I].Key.Z) - int64(BucketKey.Z);
		const int64 DistSq = Dx * Dx + Dy * Dy + Dz * Dz;
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = I;
		}
	}
	UE_LOG(LogVoxelWater, Warning,
	       TEXT("voxel.Water.GPU: %d water sort buckets live (cap %d); brick bucket (%lld,%lld,%lld) folded into "
	            "bucket %d. Translucent sorting degrades to pre-W5 for that region -- geometry is unaffected."),
	       Impl.PoolBuckets.Num(), kMaxWaterPoolBuckets, (long long)BucketKey.X, (long long)BucketKey.Y,
	       (long long)BucketKey.Z, Best);
	return Best;
}

// The pooled equivalent of "destroy this brick's component". Safe to call for a
// brick that was never pooled.
void ReleaseWaterBrickPooled(FVoxelWaterImpl& Impl,
                             TMap<VoxelCoords::FVoxelCoord, FVoxelWaterImpl::FPooledBrick>& Slots,
                             const VoxelCoords::FVoxelCoord& BrickCoord)
{
	FVoxelWaterImpl::FPooledBrick Slot;
	if (!Slots.RemoveAndCopyValue(BrickCoord, Slot) || Slot.Handle == INDEX_NONE)
	{
		return;
	}
	// Released through the bucket the handle was ISSUED BY, which the record
	// carries. Re-deriving it from BrickCoord would be wrong the moment a bucket
	// is re-homed: the brick's key would name a bucket that is now somewhere
	// else, and RemoveChunk would free a stranger's range.
	if (UVoxelGpuPoolComponent* Pool = GetBucketPool(Impl, Slot.Bucket))
	{
		Pool->RemoveChunk(Slot.Handle);
	}
}

// Puts one brick's quads into the pool, adding or re-meshing in place.
//
// Returns false if the pool refused the geometry, in which case the brick is
// left undrawn rather than half-drawn -- and the caller must NOT then fall back
// to a component for it, because a brick drawn twice is worse than a brick
// drawn once.
bool ApplyWaterBrickPooled(FVoxelWaterImpl& Impl, AActor* ChunkOwner, UMaterialInterface* Material,
                           TMap<VoxelCoords::FVoxelCoord, FVoxelWaterImpl::FPooledBrick>& Slots,
                           const VoxelCoords::FVoxelCoord& BrickCoord,
                           const FVector& BrickOriginUU,
                           const TArray<FVoxelChunkQuad>& Quads,
                           const TArray<uint32>& CornerHeights,
                           float Activity)
{
	// One packed corner word per quad, always -- EmitWaterQuads fills both arrays
	// in lockstep, including on the split path. The pool indexes them by the same
	// slot offset, so a length mismatch would silently shift every corner height
	// by however many quads had been dropped.
	check(CornerHeights.Num() == Quads.Num());

	// A RESIDENT BRICK STAYS IN THE BUCKET THAT ISSUED ITS HANDLE, even if that
	// bucket has since been re-homed elsewhere. Moving it would mean a free in
	// one pool and an alloc in another with the geometry visible in neither for
	// a frame, to fix a sort key -- and a brick only ends up in a foreign bucket
	// via the at-cap fallback, which is a degraded case by construction.
	FVoxelWaterImpl::FPooledBrick* ExistingSlot = Slots.Find(BrickCoord);
	const bool bResident = ExistingSlot != nullptr && ExistingSlot->Handle != INDEX_NONE
		&& GetBucketPool(Impl, ExistingSlot->Bucket) != nullptr;

	const int32 BucketIndex = bResident
		? ExistingSlot->Bucket
		: GetOrCreateWaterPoolBucket(Impl, ChunkOwner, Material, BrickCoord);
	UVoxelGpuPoolComponent* Pool = GetBucketPool(Impl, BucketIndex);
	if (Pool == nullptr)
	{
		return false;
	}

	// The CPU mesher's quads in the 8 bytes the pool stores. Water bricks are
	// already 0..7 on every axis, so unlike terrain there is no brick-to-chunk
	// rebase to bake in first -- brick-local IS entry-local here.
	TArray<uint64> Packed;
	Packed.SetNumUninitialized(Quads.Num());
	for (int32 I = 0; I < Quads.Num(); ++I)
	{
		Packed[I] = PackVoxelChunkQuad(Quads[I]);
	}

	// Params: xy = terrain's climate pair, z = terrain's surface gate, w spare.
	// Water uses exactly ONE of them.
	//
	// .y IS THE FOAM ACTIVITY (W5). The vertex factory writes ChunkParams.y
	// straight into vertex colour A under every mode
	// (VoxelQuadVertexFactory.ush: `Intermediates.Color = half4(..., ChunkClimate.y)`),
	// so this reaches M_WaterVoxel with ZERO shader plumbing -- no new uniform
	// member, and therefore no exposure to the loose-FShaderParameter trap that
	// silently no-ops in this vertex factory (docs/gpu-g2-draw-path.md).
	// .x stays at terrain's neutral midpoint and .z at kNoSurfaceGate
	// ("everything counts as surface"), which is the same well-defined fallback
	// the pool gives its own hidden entry; neither reaches this material.
	const FVector4f Params(0.5f, FMath::Clamp(Activity, 0.0f, 1.0f),
	                       UVoxelGpuPoolComponent::kNoSurfaceGate, 0.0f);

	int32 NewSlot;
	if (bResident)
	{
		// PARAMS FIRST, THEN THE RE-MESH. SetChunkParams only marks the chunk
		// table dirty and defers; UpdateChunk's own PushUpdatesToProxy then
		// carries the new row in the same publication. The other order would
		// leave activity trailing the geometry by one flush -- which, for a
		// brick that just went still, means one tick of foam on settled water.
		Pool->SetChunkParams(ExistingSlot->Handle, Params);
		// Re-mesh in place. This is water's COMMON case, not its rare one: every
		// active brick re-meshes at the 10 Hz CA cadence, so free+realloc on each
		// tick would fragment the pool far faster than terrain digging does.
		NewSlot = Pool->UpdateChunk(ExistingSlot->Handle, Packed, CornerHeights);
	}
	else
	{
		NewSlot = Pool->AddChunk(
			Packed, FVector3f(BrickOriginUU - Impl.PoolBuckets[BucketIndex].Rebase), /*Level=*/0, Params,
			&CornerHeights);
	}

	if (NewSlot == INDEX_NONE)
	{
		// Out of contiguous room. Drop the mapping so the brick is honestly
		// undrawn rather than pointing at a range that is no longer its own.
		Slots.Remove(BrickCoord);
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("voxel.Water.GPU: no room for %d quads in bucket %d (%u free, largest run %u). "
		            "Brick left undrawn."),
		       Packed.Num(), BucketIndex, Pool->GetFreeQuads(), Pool->GetLargestFreeRun());
		return false;
	}

	Slots.Add(BrickCoord, FVoxelWaterImpl::FPooledBrick{BucketIndex, NewSlot});
	return true;
}

// Doctrine SS2.5 ("everything expensive is budgeted, never demand-driven"):
// per-brick vxc::meshBrick<8> (re-mesh of an existing component OR meshing
// for a brand-new one -- both cost the same greedy-mesh pass) is the
// expensive per-tick operation, NOT just NewObject+RegisterComponent as
// first assumed. Verification testing measured this progressively:
//   1. Every apron sample routed through vxc::WaterCA::fillAt()'s full
//      hash+unordered_map lookup: ~90 active bricks cost 10-40ms/tick.
//   2. Fixed by indexing the already-fetched WaterBrick8 directly for
//      in-bounds samples (see the Sampler below) -- but a NEW-component
//      creation cap alone (kMaxNewComponentsPerTick, tried at 32 then 4)
//      still left 15-69ms spikes: re-meshing ~90-150 ALREADY-EXISTING
//      components every tick (uncapped) was the remaining cost, not
//      creation.
// Fix: ONE unified per-tick budget over the actual expensive operation
// (the meshBrick<8> call itself), covering both re-mesh and create alike;
// only cheap destroy-on-empty is unbudgeted (see below). A sustained flood
// larger than this budget spreads its update cost over several 10Hz ticks
// (each dirty-but-deferred brick keeps its last-known mesh on screen for
// one extra tick at most) instead of spiking one of them.
constexpr int32 kMaxBrickMeshesPerTick = 8;

// The smallest CA fill a cell must hold before it is meshed at all.
//
// The meshing sampler returns the fill fraction itself as the vxc::MaterialId
// (see the Sampler lambda below), and vxc::MAT_AIR is 0, so ANY nonzero fill
// would otherwise produce geometry. At the extreme that means a cell holding
// 1/255 units emits a full quad whose top face M_WaterVoxel then pushes down
// to 0.4 mm above the floor -- six vertices and a translucent draw for
// something thinner than the mesher's own coordinate resolution.
//
// This is a RENDERING floor only. The CA still simulates, conserves and
// replicates those units exactly as before; they are simply not drawn until
// there is enough to see. That distinction matters because the alternative --
// clamping the fill itself -- would destroy volume, which is the one thing
// waterca.h's ledger is built to make impossible.
//
// 8/255 is ~3% of a voxel, i.e. ~3 mm at kVoxelSizeMm = 100 -- below what a
// player can notice missing, above the sliver regime described here.
//
// ONE THING TO REVISIT IF THE SWE COUPLER IS EVER ENABLED (ADR-0004): a
// settled SWE surface is flat only to its derived +/-16-unit deadband, so a
// cell sitting near this floor could cross it repeatedly and flicker in and
// out of the mesh. Hysteresis (or a floor below the deadband) would be the fix.
// The CA alone settles to +/-1 unit, so the problem does not arise today.
constexpr uint8_t kMinVisibleFill = 8;

// Height quantisation for the MERGE byte only (see QuantiseFillForMesh).
//
// Fill-as-material makes the mesher's merge key carry the fill value, which is
// exactly what splits faces of differing depth -- the property that produces a
// stepped surface. The cost is that it also refuses to merge cells that differ
// by a single unit, and the CA settles "flat within +/-1", so a settled pool
// fragments toward one quad per surface cell. MEASURED on the standard
// 30,000-unit pour: 365 quads through the old fill>=128 binarisation vs 984
// with raw fill-as-material, i.e. 2.7x, and that multiple scales with the
// water body rather than being a fixed cost.
//
// Snapping the merge byte to 8-unit buckets makes those +/-1 neighbours agree
// again while costing ~3.2 mm of height resolution (8/255 of a 10 cm voxel) --
// still ~30x finer than the 10 cm lattice, and far below anything visible.
// Occupancy is deliberately NOT quantised: the kMinVisibleFill test upstream
// still sees the exact fill, so thin water does not change visibility.
constexpr uint8_t kFillMergeBucket = 8;

// Near-full cells snap UP to 255 rather than down to a bucket edge, so the
// interior of a settled body (which sits at 254-255) merges into one large
// quad instead of splitting against a 248 bucket.
constexpr uint8_t kFillFullSnap = 250;

inline vxc::MaterialId QuantiseFillForMesh(uint8_t Fill)
{
	if (Fill >= kFillFullSnap)
	{
		return vxc::MaterialId(255);
	}
	const uint8_t Bucketed = uint8_t(Fill & ~(kFillMergeBucket - 1));
	// Never quantise a visible cell down below the visibility floor -- that
	// would delete thin water the exact-fill test just admitted.
	return vxc::MaterialId(Bucketed < kMinVisibleFill ? kMinVisibleFill : Bucketed);
}

// --- C0 BILINEAR WATER SURFACE ----------------------------------------------
//
// WHAT THIS REPLACES. The stepped surface above seats each surface cell's top
// face at ITS OWN fill height, which is correct per cell and wrong per body:
// the height is constant across a face and jumps at every cell boundary, so a
// settled pool reads as a field of 10 cm plateaus rather than a water surface.
// At the CA's own +/-1-unit settle tolerance those jumps are invisible, but a
// pour, a wave front or a draining lake routinely holds neighbours 30-80 fill
// units apart, and that is 1-3 cm of vertical step every 10 cm laterally.
//
// AND IT CLOSES AN ACTUAL HOLE, not only a cosmetic one. voxel-core's mesher
// emits NO face between two occupied cells (mesher.h: the face mask requires
// !solidAt(neighbour)), so two adjacent partial cells of different fill get
// tops at different heights with no riser between them -- an open slit up to a
// whole voxel tall, straight through to the water's interior, on exactly the
// active cascades where the fill difference is largest. That is not fixable by
// tuning the step size; it needs the two tops to MEET.
//
// THE FIX IS PER-CORNER HEIGHTS, SHARED BETWEEN NEIGHBOURS. Each vertex of a
// water face sits on a LATTICE CORNER of the (x,y) grid, where four columns
// meet. Give that corner the average of those four columns' surface heights and
// two adjacent cells necessarily agree along their shared edge, because they
// average the same four columns -- so the surface is C0 by construction and the
// slit cannot exist. Interpolation across the quad then does the rest: the top
// is a bilinear patch instead of a plateau.
//
// The height still rides VertexColor.R through M_WaterVoxel's existing World
// Position Offset ((1 - R) * one voxel down, gated on the per-vertex
// top-boundary flag in B). Nothing about that formula changes; R simply becomes
// PER-CORNER rather than per-face. So the material asset is untouched, and both
// render paths keep the encoding they already share.
//
// WHAT IS DELIBERATELY NOT DONE: voxel-core is not modified. The mesher still
// merges by (material | ao | visible) and knows nothing about corners; where a
// merged quad cannot represent the corner field (see EmitWaterQuads) it is
// split back into unit quads HERE, on the presentation side, so determinism and
// the CPU/GPU mesher parity contract are untouched.

// The padded fill block one brick's mesh is built from: the brick's own 8^3
// cells plus the 1-voxel apron vxc::meshBrick documents as its sampler domain
// ([-1, B] on every axis).
//
// MATERIALISED ONCE PER BRICK, and that costs nothing extra. meshBrick ALREADY
// materialises exactly this block before its first face scan (see its own
// "Performance note"), calling the sampler once per padded cell -- so building
// it one level up and handing the mesher a lambda that reads it back is the
// same 1,000 source reads per brick, not a second pass over the CA.
//
// It has to exist because the corner heights below need the SAME cells the
// mesher needed, in a different order (per lattice corner, not per face) and
// several times each. Re-deriving them through a live sampler would put
// vxc::WaterCA::fillAt()'s hash + unordered_map lookup back on every apron
// sample -- the exact cost the sampler's own "index the already-fetched brick"
// comment records as 10-40 ms/tick over ~90 active bricks.
// --- SWE union sampling (ADR-0004 "Renderer") -------------------------------
//
// THE REQUIREMENT. Once the coupler promotes a column, that column's water
// lives in the SHEET, not in CA cells -- measured: a 30,000-unit pour ends up
// 29,780 in the sheet and 220 in the CA. The CA sampler alone therefore draws
// 0.7% of the water and the pour looks like it evaporated. ADR-0004 states the
// visible surface must be the UNION of sheet depth and CA fill.
//
// WHY THIS IS A TERM IN THE CA SAMPLER AND NOT A THIRD RENDER SOURCE. The
// implicit-cavern-water path is a genuine second source with its own component
// and pool-slot maps, and that works because implicit-vs-CA is disjoint BY
// BRICK (mobilization is per brick). SWE-vs-CA is disjoint BY VOXEL WITHIN A
// COLUMN: an SWE column's sheet occupies [bed+1, bed+sheetScanVoxels] while the
// CA still owns everything at or below the bed, and may hold a rate-limited
// residue inside that same range. "This brick belongs to source X" cannot
// express that boundary, so a third parallel source would draw two translucent
// surfaces over one brick -- exactly the doubling MarkMobilizedBricksDirty
// exists to prevent on the implicit path.
//
// WHY THE SUM, NOT max(CA, SWE). The coupler's CA-side evacuation is rate
// limited (absorbPerTick), so a fast source leaves a bounded standing residue
// inside the sheet's z-range. Drawing max() under-draws by up to that residue;
// drawing both double-draws. Summing the residue into the depth and then
// suppressing the CA's own contribution over exactly that range is exact.
struct FSweColumnDepths
{
	static constexpr int32 kPad = vxc::WaterBrick8::kEdge + 2; // 10 columns per axis

	// Effective depth (fill units) and bed voxel z, per padded column. Bed is
	// only meaningful where bHasSheet.
	int32 EffDepth[kPad * kPad] = {};
	int32 Bed[kPad * kPad] = {};
	bool bHasSheet[kPad * kPad] = {};
	int32 ScanVoxels = 0;
	bool bAnySheet = false;

	int32 Index(int32 X, int32 Y) const { return (X + 1) + kPad * (Y + 1); }
};

// Resolves the sheet for the 10x10 columns a brick's padded block spans.
//
// PER COLUMN, NOT PER VOXEL. The union needs a sum over the sheet's whole
// z-range, and the pad has 1,000 cells across only 100 columns -- computing it
// per sample would repeat the same scan ten times per column. Building it once
// costs 100 columns x sheetScanVoxels (8) CA reads, the same order as the pad
// itself, and every pad cell then resolves in O(1).
void BuildSweColumnDepths(FSweColumnDepths& Out, const FVoxelWaterImpl& Impl, const vxc::BrickKey& Key)
{
	if (!Impl.SweSheet || !Impl.SweCoupler)
	{
		return;
	}
	// sheetScanVoxels is the COUPLER's knob (it bounds how far above the bed
	// the coupler scans for CA fill to absorb), not the grid's -- and it must
	// be read from there rather than duplicated, because the union's
	// suppression range has to be exactly the range the coupler absorbs over.
	// A local copy that drifted would either double-draw or lose a layer.
	Out.ScanVoxels = Impl.SweCoupler->config().sheetScanVoxels;

	for (int32 Cy = -1; Cy <= vxc::WaterBrick8::kEdge; ++Cy)
	{
		for (int32 Cx = -1; Cx <= vxc::WaterBrick8::kEdge; ++Cx)
		{
			const int64_t Vx = int64_t(Key.x) * vxc::WaterBrick8::kEdge + Cx;
			const int64_t Vy = int64_t(Key.y) * vxc::WaterBrick8::kEdge + Cy;
			if (!Impl.SweSheet->inBounds(Vx, Vy) || !Impl.SweCoupler->isSweColumn(Vx, Vy))
			{
				continue; // CA owns this column outright
			}

			const int32 Bed = Impl.SweSheet->bedAt(Vx, Vy);
			int64_t Eff = Impl.SweSheet->depthAt(Vx, Vy);
			// The CA residue inside the sheet's own range, folded in.
			for (int32 K = 1; K <= Out.ScanVoxels; ++K)
			{
				Eff += Impl.CA.fillAt(Vx, Vy, int64_t(Bed) + K);
			}

			const int32 I = Out.Index(Cx, Cy);
			Out.bHasSheet[I] = true;
			Out.Bed[I] = Bed;
			Out.EffDepth[I] = int32(FMath::Min<int64_t>(Eff, int64_t(Out.ScanVoxels) * 255));
			Out.bAnySheet = true;
		}
	}
}

// The per-voxel union. CaFill is what the CA sampler would have returned.
//
// Stacking convention is WaterCA::addWater's and SweCaCoupler::demote()'s, and
// deliberately so: a demotion writes the sheet back into CA cells with exactly
// this layout, so hand-back is invisible instead of a pop.
uint8 UnionSweFill(const FSweColumnDepths& Sheet, int32 PadX, int32 PadY, int64_t Vz, uint8 CaFill)
{
	if (!Sheet.bAnySheet)
	{
		return CaFill;
	}
	const int32 I = Sheet.Index(PadX, PadY);
	if (!Sheet.bHasSheet[I])
	{
		return CaFill;
	}

	const int64_t K = Vz - int64_t(Sheet.Bed[I]); // 1 == first voxel above the bed
	if (K < 1 || K > Sheet.ScanVoxels)
	{
		// At or below the bed, or above the sheet's reach: CA owns it outright.
		return CaFill;
	}
	// Inside the sheet's range the CA's own contribution is SUPPRESSED -- it was
	// already summed into EffDepth, and returning it here as well is the
	// double-draw this whole structure exists to avoid.
	const int64_t Remaining = int64_t(Sheet.EffDepth[I]) - 255 * (K - 1);
	const uint8 Slice = uint8(FMath::Clamp<int64_t>(Remaining, 0, 255));

	// THE FLICKER FIX (the revisit kMinVisibleFill's comment asks for).
	//
	// A settled SWE surface is flat only to the coupler's derived +/-16-unit
	// deadband, against the CA's +/-1. Thresholding the SLICE at kMinVisibleFill
	// = 8 therefore has a cell whose remainder sits near 8 crossing the floor
	// every tick, appearing and vanishing -- and no floor value fixes that,
	// because an oscillation of +/-16 crosses ANY single threshold it straddles.
	// Per-cell hysteresis would need a 512-bit "currently drawn" mask per brick
	// and a band wider than the deadband to actually be stable.
	//
	// The cheaper and more honest fix is to move the DECISION to the quantity
	// that is stable. The unstable number is the top slice's remainder, which is
	// a modulus and lives in [0,255) no matter how much water the column holds.
	// The column's total depth is not: a column holding 100 units still reads
	// 84-116 under the same deadband, nowhere near the floor. So visibility is
	// decided ONCE PER COLUMN on EffDepth, and a column that qualifies draws its
	// top slice at the floor rather than dropping it.
	//
	// Costs at most 8/255 of a voxel (~3 mm) of over-draw on that one slice, and
	// only where the true remainder was already below what a player can see. No
	// volume is touched: this is the render path, and the ledger never sees it.
	if (Slice > 0 && Slice < kMinVisibleFill && Sheet.EffDepth[I] >= kMinVisibleFill)
	{
		return kMinVisibleFill;
	}
	return Slice;
}

struct FWaterFillPad
{
	static constexpr int32 kEdge = vxc::WaterBrick8::kEdge;   // 8
	static constexpr int32 kPad = kEdge + 2;                  // 10: [-1 .. 8]

	uint8 Fill[kPad * kPad * kPad];

	// Brick-local, and -1 / kEdge (the apron) are legal on every axis.
	uint8 At(int32 X, int32 Y, int32 Z) const
	{
		checkSlow(X >= -1 && X <= kEdge && Y >= -1 && Y <= kEdge && Z >= -1 && Z <= kEdge);
		return Fill[(X + 1) + kPad * ((Y + 1) + kPad * (Z + 1))];
	}
};

// Fills a pad from any per-cell RAW fill source, exactly once per padded cell.
// The two producers (the CA's WaterBrick8 + apron, and the mobilizer's implicit
// flood field) differ only in this functor.
//
// Z IS THE INNERMOST LOOP, AND THAT IS THE WHOLE POINT OF THIS FUNCTION'S
// SHAPE. It used to be the outermost, chosen so the writes were a straight
// append into the Z-major layout below instead of a scattered store. That
// traded a few hundred nanoseconds of store locality for a catastrophe,
// because the implicit producer's functor is not a memory read -- it is a
// worldgen query, and EVERY layer between here and worldgen memoises exactly
// one column:
//
//   * vxc::Amplifier::columnCached  -- "valid until this thread's next
//     columnCached call" (amplifier.h). Reached TWICE per cell, through two
//     different Amplifier instances that do not share a memo: once via
//     UVoxelWorldSubsystem::IsSolidAtVoxel -> World::materialAt ->
//     Amplifier::materialAt(vx,vy,vz), and once via the ImplicitFn's own
//     `Amp.columnCached(vx, vy).cavern`.
//   * FVoxelWaterImpl::GroundMmAt   -- one entry, and it also fires
//     FVoxelFineTileStreamer::RequestFootprint on every miss.
//   * vxc::LakeSampler              -- one entry, on the fine PIXEL.
//
// A one-entry memo hits only when consecutive calls share (vx, vy). With Z
// outermost, X changes on EVERY call, so all four memos missed on all 1,000
// cells of every brick: three full amplifier columns and a streamer footprint
// request per cell, ~1.4 us each, ~1.4 ms per brick. At the 192-brick tick
// budget that is ~277 ms of game thread per tick and ~59 s to drain one
// implicit refresh of a lake -- which is why a baked lake rendered as scattered
// bricks rather than a sheet: the candidate list never came close to draining
// before the shot, and any 0.8 m of camera movement restarted it.
//
// With Z innermost the same 1,000 cells touch 100 columns, each ten times in a
// row, and every one of those memos does the job its own comment claims. The
// scattered store through At() is the correct trade at three orders of
// magnitude. Measured cost of one amplifier column: 0.469 us (vxc_bench,
// brick 8, 640,000 columns in 300.1 ms).
//
// The CA producer is indifferent to the order -- it reads a resident brick --
// so this is one loop nest for both rather than a second specialised one.
template <typename FillFn>
void BuildWaterFillPad(FWaterFillPad& OutPad, const FillFn& Fn)
{
	for (int32 Y = -1; Y <= FWaterFillPad::kEdge; ++Y)
	{
		for (int32 X = -1; X <= FWaterFillPad::kEdge; ++X)
		{
			for (int32 Z = -1; Z <= FWaterFillPad::kEdge; ++Z)
			{
				OutPad.Fill[(X + 1) + FWaterFillPad::kPad * ((Y + 1) + FWaterFillPad::kPad * (Z + 1))] =
					Fn(X, Y, Z);
			}
		}
	}
}

// The water surface height at every LATTICE CORNER of the brick, for every cell
// layer, as a 0..255 fraction of THAT layer's own cell.
//
// 9 x 9 x 8 = 648 bytes, built once per brick and then read by every quad. The
// alternative -- computing four corners per quad on demand -- re-derives the
// same lattice point up to four times (once per touching face) and, worse,
// makes the split test below quadratic in the merge size.
//
// WHY THE HEIGHT IS EXPRESSED WITHIN A CELL rather than as an absolute Z. The
// quad packing cannot carry a fractional face position (VoxelQuadDecode.ush
// computes FaceCoordVox = Slice + Positive in integers), so the height must
// reach the material as the 0..1 WPO drop it already understands, which is
// cell-relative by definition. That clamp is also what keeps the surface
// watertight where it crosses a cell boundary: a corner whose true height is in
// the cell ABOVE clamps to 255 (this cell's ceiling) while the cell above
// clamps it to its own fraction, and the riser between them is a real side face
// -- the mesher emits one there precisely because the cell beside it is air.
struct FWaterCornerField
{
	static constexpr int32 kLat = vxc::WaterBrick8::kEdge + 1;      // 9 corners per axis
	static constexpr int32 kLayers = vxc::WaterBrick8::kEdge;       // 8 cell layers

	uint8 H[kLat * kLat * kLayers];

	uint8 At(int32 Lx, int32 Ly, int32 Cz) const
	{
		checkSlow(Lx >= 0 && Lx < kLat && Ly >= 0 && Ly < kLat && Cz >= 0 && Cz < kLayers);
		return H[Lx + kLat * (Ly + kLat * Cz)];
	}
};

void BuildWaterCornerField(const FWaterFillPad& Pad, FWaterCornerField& Out)
{
	for (int32 Cz = 0; Cz < FWaterCornerField::kLayers; ++Cz)
	{
		for (int32 Ly = 0; Ly < FWaterCornerField::kLat; ++Ly)
		{
			for (int32 Lx = 0; Lx < FWaterCornerField::kLat; ++Lx)
			{
				// The four columns meeting at this lattice corner. Every read is
				// in [-1, 8] on each axis (Lx-1 >= -1, Lx <= 8, Cz+1 <= 8), i.e.
				// inside the pad the mesher already required.
				int32 Sum = 0;
				int32 Count = 0;
				for (int32 Dy = -1; Dy <= 0; ++Dy)
				{
					for (int32 Dx = -1; Dx <= 0; ++Dx)
					{
						const int32 Cx = Lx + Dx;
						const int32 Cy = Ly + Dy;

						// This column continues above the layer, so its surface is
						// not here: it is full to this cell's ceiling.
						if (Pad.At(Cx, Cy, Cz + 1) >= kMinVisibleFill)
						{
							Sum += 255;
							++Count;
							continue;
						}

						const uint8 F = Pad.At(Cx, Cy, Cz);
						if (F >= kMinVisibleFill)
						{
							Sum += int32(F);
							++Count;
						}
						// ELSE: A DRY COLUMN CONTRIBUTES NOTHING -- it does not
						// average in as height 0. This is the difference between a
						// pool that meets its containing wall flat and one whose
						// surface droops to the floor at every boundary. Water
						// against rock has no lower neighbour to blend toward: the
						// absent column is absent, not empty. A genuine shoreline
						// still tapers, because there the neighbouring columns hold
						// real, smaller fills and DO average in.
					}
				}

				// Count is >= 1 for every corner any quad can reference (a quad's
				// own cell is always one of the four columns of each of its
				// corners), so the fallback is unreachable from the emit path. 255
				// -- "full height, do not move this vertex" -- is the harmless
				// direction if it ever is reached.
				Out.H[Lx + FWaterCornerField::kLat * (Ly + FWaterCornerField::kLat * Cz)] =
					Count > 0 ? uint8((Sum + Count / 2) / Count) : uint8(255);
			}
		}
	}
}

// Which lattice corner of the surface field a point on a face plane maps to.
//
// Uc/Vc are the quad's own in-plane coordinates (u = (axis+1)%3, v =
// (axis+2)%3). Only ever called for a TOP-boundary point, which is what makes
// the Vc-1 / Uc-1 below safe: those subtract 1 from a Z LATTICE coordinate to
// name the cell layer underneath it, and a top-boundary point always has that
// coordinate >= 1.
uint8 CornerHeightAtFacePoint(const FWaterCornerField& Field, int32 Axis, int32 Positive, int32 Slice,
                              int32 Uc, int32 Vc)
{
	if (Axis == 2)
	{
		// u = x, v = y; the whole quad lies in one cell layer.
		return Field.At(Uc, Vc, Slice);
	}
	if (Axis == 0)
	{
		// u = y, v = z. The face plane is at x = Slice + Positive.
		return Field.At(Slice + Positive, Uc, Vc - 1);
	}
	// Axis == 1: u = z, v = x. The face plane is at y = Slice + Positive.
	return Field.At(Vc, Slice + Positive, Uc - 1);
}

// Which of a quad's four corners sit on the TOP (+Z) boundary of the cell the
// face belongs to, as a bitmask over the corner order the two proxies and
// VoxelQuadDecode.ush all share: 0=(u0,v0) 1=(u0,v1) 2=(u1,v1) 3=(u1,v0).
//
// This is the same question FWaterChunkSceneProxy's bTopCorner and
// DecodeVoxelQuadVertex's TopBoundary answer, expressed against the quad fields
// instead of against decoded positions -- all three must agree, because B (the
// gate) comes from those two and R (the height) comes from this one.
uint8 TopCornerMaskFor(int32 Axis, int32 Positive)
{
	if (Axis == 2)
	{
		return Positive != 0 ? 0xFu : 0x0u;   // one plane: all four, or none
	}
	if (Axis == 0)
	{
		return 0x6u;   // v carries z -> corners 1 (u0,v1) and 2 (u1,v1)
	}
	return 0xCu;       // axis 1: u carries z -> corners 2 (u1,v1) and 3 (u1,v0)
}

// The quad's four corner heights, packed one byte per corner in that same
// position order (byte 0 = corner 0). NOT the AO order -- see
// VoxelQuadDecode.ush's AoShiftForCorner, which exists precisely because
// vxc::detail::aoCorner packs (0,0),(1,0),(0,1),(1,1). Choosing position order
// here means the shader's shift is Corner*8 with no remap table, and the one
// place the two orders could be confused is this comment.
//
// A corner that is NOT on the top boundary keeps the quad's own `mat` byte,
// which is exactly what R held for every vertex before this change. The WPO is
// gated to zero on those vertices, so the value is unused either way -- but
// preserving it means a bottom or lower-side vertex's R is byte-identical to
// the stepped-surface build, and any future reader of R sees no new case.
uint32 PackQuadCornerHeights(const FWaterCornerField& Field, const FVoxelChunkQuad& Q)
{
	const uint8 TopMask = TopCornerMaskFor(Q.Axis, Q.Positive);
	const int32 U0 = int32(Q.U0), V0 = int32(Q.V0);
	const int32 U1 = U0 + int32(Q.W), V1 = V0 + int32(Q.H);
	const int32 Us[4] = {U0, U0, U1, U1};
	const int32 Vs[4] = {V0, V1, V1, V0};

	uint32 Packed = 0;
	for (int32 C = 0; C < 4; ++C)
	{
		const uint8 Byte = (TopMask & (1u << C)) != 0
			? CornerHeightAtFacePoint(Field, Q.Axis, Q.Positive, Q.Slice, Us[C], Vs[C])
			: Q.Mat;
		Packed |= uint32(Byte) << (8 * C);
	}
	return Packed;
}

// How far, in 1/255ths of a voxel, an interior lattice corner may deviate from
// what a merged quad's own interpolation produces before the quad is split.
//
// 2/255 of a 10 cm voxel is 0.8 mm. The merge itself already accepts 8/255
// (3.2 mm) of height error by quantising the merge key (kFillMergeBucket), so
// this is well inside the error budget the stepped surface already spends -- it
// exists to catch REAL discontinuities (a pool edge, a wall, a wave front),
// not the +/-1-unit noise of a settled body, which would otherwise split every
// quad in a still pool and cost the merge for nothing.
constexpr int32 kCornerPlanarToleranceUnits = 2;

// Can this quad's interpolation reproduce the corner field across its whole
// extent?
//
// THE QUESTION MATTERS BECAUSE A MERGED QUAD HAS NO INTERIOR VERTICES. Its four
// corners are its only samples, so a 4-wide surface quad draws a straight line
// between the lattice heights at its two ends and ignores the three in between
// -- while the neighbouring quads along that edge (a +Z face of the next cell
// layer, or the side face of the cell beyond it) DO have vertices there. Where
// the field is not planar over the merge, that is a crack: the exact defect
// per-corner heights are here to remove.
//
// Two triangles interpolate a PLANE exactly and a general bilinear patch only
// when the corners happen to be coplanar (a triangle pair is linear on each
// half of the A-C diagonal), so the test is planarity, evaluated in integers
// against every lattice point the quad spans:
//
//   Pred(i,j) = A + (D - A) * i/w + (B - A) * j/h        [A=c0 B=c1 C=c2 D=c3]
//
// scaled by w*h to stay exact. Corner C is itself one of the lattice points
// tested (i=w, j=h), so 4-corner coplanarity is included rather than assumed.
bool QuadInterpolationCoversCornerField(const FWaterCornerField& Field, const FVoxelChunkQuad& Q)
{
	const int32 U0 = int32(Q.U0), V0 = int32(Q.V0);
	const int32 W = int32(Q.W), H = int32(Q.H);

	if (Q.Axis == 2)
	{
		if (Q.Positive == 0 || (W == 1 && H == 1))
		{
			return true;   // no top-boundary corners, or nothing between them
		}
		const int32 A = int32(Field.At(U0, V0, Q.Slice));
		const int32 B = int32(Field.At(U0, V0 + H, Q.Slice));
		const int32 D = int32(Field.At(U0 + W, V0, Q.Slice));
		const int32 Den = W * H;
		for (int32 J = 0; J <= H; ++J)
		{
			for (int32 I = 0; I <= W; ++I)
			{
				const int32 Actual = int32(Field.At(U0 + I, V0 + J, Q.Slice));
				const int32 Pred = A * Den + (D - A) * I * H + (B - A) * J * W;
				if (FMath::Abs(Actual * Den - Pred) > kCornerPlanarToleranceUnits * Den)
				{
					return false;
				}
			}
		}
		return true;
	}

	// A side face's top boundary is a single EDGE, so the test is 1D along
	// whichever in-plane axis is horizontal. The other axis carries z, and the
	// corners below the top edge never move at all -- a merged wall spanning
	// several cells vertically is still exact, which is what keeps a deep pool's
	// walls as cheap as they are today.
	if (Q.Axis == 0)
	{
		if (W == 1)
		{
			return true;
		}
		const int32 P0 = int32(CornerHeightAtFacePoint(Field, 0, Q.Positive, Q.Slice, U0, V0 + H));
		const int32 P1 = int32(CornerHeightAtFacePoint(Field, 0, Q.Positive, Q.Slice, U0 + W, V0 + H));
		for (int32 I = 0; I <= W; ++I)
		{
			const int32 Actual = int32(CornerHeightAtFacePoint(Field, 0, Q.Positive, Q.Slice, U0 + I, V0 + H));
			if (FMath::Abs(Actual * W - (P0 * W + (P1 - P0) * I)) > kCornerPlanarToleranceUnits * W)
			{
				return false;
			}
		}
		return true;
	}

	if (H == 1)
	{
		return true;
	}
	const int32 P0 = int32(CornerHeightAtFacePoint(Field, 1, Q.Positive, Q.Slice, U0 + W, V0));
	const int32 P1 = int32(CornerHeightAtFacePoint(Field, 1, Q.Positive, Q.Slice, U0 + W, V0 + H));
	for (int32 J = 0; J <= H; ++J)
	{
		const int32 Actual = int32(CornerHeightAtFacePoint(Field, 1, Q.Positive, Q.Slice, U0 + W, V0 + J));
		if (FMath::Abs(Actual * H - (P0 * H + (P1 - P0) * J)) > kCornerPlanarToleranceUnits * H)
		{
			return false;
		}
	}
	return true;
}

// Turns one brick's greedy-mesh output into the render arrays, splitting any
// quad whose merge the corner field cannot survive.
//
// OutCorners is PARALLEL to OutQuads, one packed uint32 per quad, and both
// paths carry it beside the quads rather than inside them: FVoxelChunkQuad and
// the 8-byte packed form are a contract shared with the GPU mesher and the
// terrain renderer (VoxelMeshTypes.h, VoxelQuadDecode.ush), and the packed word
// is full. A parallel array costs 4 bytes per quad on the water path only and
// changes nothing terrain touches.
//
// SPLIT SHAPES, and why they are not all "unit quads":
//   * +Z surface faces split both ways -- the corner field varies in x and y
//     and only a unit cell can carry all four of its corners.
//   * side faces split only along their HORIZONTAL axis. Their vertical extent
//     has no top-boundary corners below the top row, so a 1x8 wall strip is as
//     exact as eight 1x1 ones and eight times cheaper.
//
// AO SURVIVES THE SPLIT UNCHANGED IN PRACTICE, which is worth stating because
// splitting a greedy quad usually does not: the mesher's merge key includes the
// 4-corner AO byte, so every cell of a merged quad carried the SAME byte, and
// each sub-quad inherits it. The one real difference is that a non-uniform AO
// byte's corner-to-corner ramp is then repeated per cell instead of stretched
// across the merge. For water surfaces that cannot bite: AO here is occlusion
// by OTHER WATER (the mesher's solidAt reads the fill-as-material array), and a
// free surface has air above it, so a +Z water face's AO byte is 0xFF -- four
// equal corners, no ramp to repeat.
void EmitWaterQuads(const std::vector<vxc::Quad>& RawQuads, const FWaterCornerField& Field,
                    TArray<FVoxelChunkQuad>& OutQuads, TArray<uint32>& OutCorners)
{
	OutQuads.Reserve(int32(RawQuads.size()));
	OutCorners.Reserve(int32(RawQuads.size()));

	for (const vxc::Quad& RQ : RawQuads)
	{
		FVoxelChunkQuad CQ;
		CQ.Axis = RQ.axis;
		CQ.Positive = RQ.positive;
		CQ.Slice = RQ.slice;
		CQ.U0 = RQ.u0;
		CQ.V0 = RQ.v0;
		CQ.W = RQ.w;
		CQ.H = RQ.h;
		CQ.Ao = RQ.ao;
		CQ.Mat = RQ.mat;

		if (QuadInterpolationCoversCornerField(Field, CQ))
		{
			OutQuads.Add(CQ);
			OutCorners.Add(PackQuadCornerHeights(Field, CQ));
			continue;
		}

		const int32 SubW = (CQ.Axis == 1) ? int32(CQ.W) : 1;
		const int32 SubH = (CQ.Axis == 0) ? int32(CQ.H) : 1;
		for (int32 J = 0; J < int32(CQ.H); J += SubH)
		{
			for (int32 I = 0; I < int32(CQ.W); I += SubW)
			{
				FVoxelChunkQuad Sub = CQ;
				Sub.U0 = uint8(int32(CQ.U0) + I);
				Sub.V0 = uint8(int32(CQ.V0) + J);
				Sub.W = uint8(SubW);
				Sub.H = uint8(SubH);
				OutQuads.Add(Sub);
				OutCorners.Add(PackQuadCornerHeights(Field, Sub));
			}
		}
	}
}

// Rebuilds (or destroys) the render component for every brick in
// Impl.DirtyBricks, then clears that set (except for any budget-deferred
// entries, left in place for the next call). Shared by the authority tick
// path and the client replication-receive path (task item 4's mesher applies
// identically regardless of which side produced the fill data).
void RemeshDirtyBricks(FVoxelWaterImpl& Impl, AActor* ChunkOwner, USceneComponent* ChunkRoot, UMaterialInterface* Material)
{
	if (!ChunkOwner || !ChunkRoot)
	{
		// No viewport (NM_DedicatedServer) -- simulate only, never render;
		// skip the meshBrick<8> work entirely rather than pay its cost for
		// output nobody will ever see, and drop the backlog so it can't grow
		// unbounded over a long-running dedicated server session.
		Impl.DirtyBricks.Reset();
		return;
	}

	const TArray<VoxelCoords::FVoxelCoord> ToProcess = Impl.DirtyBricks.Array();
	Impl.DirtyBricks.Reset();
	int32 MeshesThisTick = 0;
	int32 DeferredCount = 0;

	// S1-1: the water pool is batched too, and this is a DELIBERATE decision
	// rather than an inherited one (docs/speculative-generation-plan.md asks for
	// it to be stated either way).
	//
	// BATCHED, for the same reason terrain is: this loop calls AddChunk /
	// UpdateChunk / RemoveChunk once per dirty brick, each of which ends in its
	// own PushUpdatesToProxy, and water re-meshes at 10 Hz with bricks appearing
	// and vanishing continuously -- the churn that made FreeChunkIds necessary in
	// the first place. Per-brick publication is exactly the tax S0 measured on
	// the terrain side.
	//
	// AND IT IS STRICTLY SAFER HERE. The whole hazard batching introduces is the
	// same-frame free-then-GPU-write race that UnmarkQuadsDirty exists to close,
	// and it needs a PENDING GPU WRITE to occur at all. Water only ever reaches
	// the pool through AddChunk/UpdateChunk -- the CPU path, which writes the
	// shadow and marks it dirty. Nothing in this subsystem calls AddChunkFromGpu,
	// so PendingGpuWrites is always empty for this instance and the race has no
	// way to happen. If that ever changes, this scope inherits the terrain fix
	// automatically, because the subtract lives in AddChunkFromGpu itself.
	//
	// The component path below (the non-pooled arm) is untouched by this: it owns
	// UWaterChunkComponents, not pool ranges.
	//
	// W5: ONE SCOPE PER BUCKET, and one that adopts buckets created inside the
	// loop. Splitting the pool into several primitives split the publication
	// with it, so a single scope over a single pool would silently have stopped
	// batching most of the work -- the failure mode being a perf regression with
	// no counter moving, since every brick would still land correctly.
	FVoxelWaterPoolBatch WaterPoolBatch(Impl);

	for (const VoxelCoords::FVoxelCoord& BrickCoord : ToProcess)
	{
		const vxc::BrickKey Key = ToBrickKey(BrickCoord);
		const vxc::WaterBrick8* Brick = Impl.CA.findBrick(Key);

		if (!Brick)
		{
			// Brick fully drained/collapsed (WaterMap's homogeneous-empty
			// collapse == absence, per waterca.h) -- drop its component
			// entirely. Cheap (no meshBrick call) -- unbudgeted, and capping
			// it would leak stale geometry.
			if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ChunkComponents.Find(BrickCoord))
			{
				if (*Existing)
				{
					(*Existing)->DestroyComponent();
				}
				Impl.ChunkComponents.Remove(BrickCoord);
			}
			// Dispatch on what the brick HOLDS, not on the cvar: a brick drawn
			// before a mid-session voxel.Water.GPU flip must unload the way it
			// loaded. Both calls are no-ops for a brick the other path owns.
			ReleaseWaterBrickPooled(Impl, Impl.PoolSlots, BrickCoord);
			continue;
		}

		if (MeshesThisTick >= kMaxBrickMeshesPerTick)
		{
			// Budget reached -- defer to the NEXT RemeshDirtyBricks call
			// rather than pay for every dirty brick's meshBrick<8> pass in
			// one frame. Whatever this brick's component currently shows
			// (its previous mesh, or nothing if brand new) stays as-is for
			// one extra tick at most (10Hz CA cadence >> this budget's
			// drain rate for any v0-scale event).
			Impl.DirtyBricks.Add(BrickCoord);
			++DeferredCount;
			continue;
		}
		++MeshesThisTick;

		// STEPPED FILL-FRACTION SURFACES (was: "fill>=128 solid for meshing
		// v0; surface cells with partial fill render at full cube v0 --
		// partial-height mesh is W5 polish").
		//
		// The sampler now returns THE FILL FRACTION ITSELF as the
		// vxc::MaterialId, which does three things at once:
		//
		//   1. OCCUPANCY. vxc::MAT_AIR is 0, so any cell at or above
		//      kMinVisibleFill is solid to the mesher and anything below it is
		//      air. This replaces the old >=128 threshold, under which a cell
		//      less than half full was invisible and a brick sitting at a
		//      uniform 100 fill meshed to zero quads and had its component
		//      destroyed outright (the "Fully solid or fully empty" branch
		//      below). Water popped in and out at the 50% mark.
		//   2. HEIGHT. The value rides the quad's `mat` byte through
		//      PackVoxelChunkQuad into VertexColor.R -- directly on the
		//      component path (FWaterChunkSceneProxy writes FColor(Q.Mat, ...)),
		//      and via FVoxelQuadVertexFactoryParameters::WaterMode on the
		//      pooled one. M_WaterVoxel's World Position Offset reads R and
		//      seats each surface cell's top face at its own fill height. The
		//      packing itself cannot carry a fractional height: the decode
		//      computes FaceCoordVox = Slice + (Positive ? 1 : 0) in integers
		//      (VoxelQuadDecode.ush), so WPO is the mechanism.
		//   3. MERGE CORRECTNESS, FOR FREE. vxc::meshBrick's greedy mask key is
		//      `material | ao<<8 | visible<<16`, so two faces merge only when
		//      their materials match. Putting fill in the material slot
		//      therefore makes the mesher refuse to merge cells of DIFFERENT
		//      fill automatically -- no new merge predicate, no mesher change.
		//      Interior and side faces (all at 255) keep merging exactly as
		//      before; only the surface layer fragments, which is precisely
		//      where the extra quads buy the stepped waterline.
		//
		// The old placeholder was MAT_ROCK, chosen because "water's translucent
		// material doesn't branch on it". That is still true -- M_WaterVoxel
		// reads R as a scalar height, never as a categorical id -- but the byte
		// is no longer arbitrary, so it must not be repurposed again without
		// updating the material.
		// Perf: meshBrick<8> samples a padded [-1,8] range per axis, but the
		// overwhelming majority of those samples land INSIDE this brick
		// (0..7 on every axis) -- for those, index straight into the
		// already-fetched Brick pointer instead of paying vxc::WaterCA::
		// fillAt()'s full waterKeyForVoxel-hash + unordered_map-lookup cost
		// per cell. Only the apron (a neighbor brick, or empty space) falls
		// back to fillAt(). Measured necessary in verification testing: with
		// every sample going through fillAt(), a single RemeshDirtyBricks
		// pass over ~90 active bricks cost 10-40ms/tick -- far over the v0
		// <2ms/frame budget -- purely from redundant hashmap lookups within
		// a brick whose contents were already in hand.
		//
		// The raw fill is materialised into the pad FIRST and the mesher reads it
		// back, rather than the sampler reaching into the brick directly. Same
		// number of source reads (meshBrick materialises this exact block itself
		// before its first face scan), and it is what lets the corner field below
		// re-read the same cells per lattice corner without touching the CA again.
		FWaterFillPad Pad;
		// The sheet for this brick's columns, resolved once (see the union
		// comment above). No sheet armed, or no promoted column here, and this
		// is inert: bAnySheet stays false and UnionSweFill returns CA fill
		// unchanged, so the un-armed path is byte-identical to before.
		FSweColumnDepths Sheet;
		BuildSweColumnDepths(Sheet, Impl, Key);

		BuildWaterFillPad(Pad, [&Impl, &Key, Brick, &Sheet](int32 x, int32 y, int32 z) -> uint8
		{
			uint8 CaFill;
			if (x >= 0 && x < vxc::WaterBrick8::kEdge && y >= 0 && y < vxc::WaterBrick8::kEdge && z >= 0 && z < vxc::WaterBrick8::kEdge)
			{
				CaFill = Brick->get(x, y, z);
			}
			else
			{
				const int64_t Vx = int64_t(Key.x) * vxc::WaterBrick8::kEdge + x;
				const int64_t Vy = int64_t(Key.y) * vxc::WaterBrick8::kEdge + y;
				const int64_t Vz = int64_t(Key.z) * vxc::WaterBrick8::kEdge + z;
				CaFill = Impl.CA.fillAt(Vx, Vy, Vz);
			}
			// Folding the union in HERE, at the pad, rather than at the mesher's
			// sampler, is what gives SWE water the bilinear corner surface for
			// free -- the corner field reads this same pad.
			const int64_t Vz = int64_t(Key.z) * vxc::WaterBrick8::kEdge + z;
			return UnionSweFill(Sheet, x, y, Vz, CaFill);
		});

		std::vector<vxc::Quad> RawQuads;
		const auto Sampler = [&Pad](int x, int y, int z) -> vxc::MaterialId
		{
			const uint8 Fill = Pad.At(x, y, z);
			return Fill >= kMinVisibleFill ? QuantiseFillForMesh(Fill) : vxc::MAT_AIR;
		};
		vxc::meshBrick<vxc::WaterBrick8::kEdge>(Sampler, RawQuads);

		// Per-corner surface heights, and the merge splits they force. Skipped
		// entirely for a brick that meshed to nothing -- which is most of them in
		// a large body, since interior bricks emit no faces at all.
		TArray<FVoxelChunkQuad> Quads;
		TArray<uint32> CornerHeights;
		if (!RawQuads.empty())
		{
			FWaterCornerField CornerField;
			BuildWaterCornerField(Pad, CornerField);
			EmitWaterQuads(RawQuads, CornerField, Quads, CornerHeights);
		}

		if (Quads.Num() == 0)
		{
			// No visible faces: the brick is fully enclosed by other water
			// (an interior brick of a large body emits nothing, which is
			// exactly what keeps a big lake affordable) or every cell sits
			// below kMinVisibleFill. Drop any stale component rather than
			// register a proxy-less one.
			//
			// This branch used to fire far more often, and wrongly: under the
			// old >=128 occupancy rule a brick whose cells all sat at, say,
			// 100 fill meshed to zero quads and had its component destroyed,
			// so genuinely-present water rendered as nothing at all. That is
			// the case the fill-fraction sampler above fixes, and a brick held
			// at uniform sub-half fill now rendering is the cheapest single
			// proof this change works.
			if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ChunkComponents.Find(BrickCoord))
			{
				if (*Existing)
				{
					(*Existing)->DestroyComponent();
				}
				Impl.ChunkComponents.Remove(BrickCoord);
			}
			ReleaseWaterBrickPooled(Impl, Impl.PoolSlots, BrickCoord);
			continue;
		}

		const FVector BrickOriginUU(double(Key.x) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU,
		                            double(Key.y) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU,
		                            double(Key.z) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU);

		// W5 FOAM ACTIVITY, and the ONE place both render paths read it, so they
		// cannot drift. 1 while vxc::WaterCA still calls this brick active,
		// 0 the moment it settles -- and the settle transition reaches here at
		// all only because StepFixed marks the bricks that LEFT the active set
		// dirty exactly once. See FVoxelWaterImpl::ActiveBricks.
		const float Activity = Impl.ActiveBricks.Contains(BrickCoord) ? 1.0f : 0.0f;

		// A brick already holding a component keeps the component path even if
		// the cvar flipped underneath it, and vice versa: mixing representations
		// for ONE brick would draw it twice. New bricks take whichever path is
		// current. Same rule ApplyMeshResult uses for terrain.
		const bool bAlreadyPooled = Impl.PoolSlots.Contains(BrickCoord);
		const bool bAlreadyComponent = Impl.ChunkComponents.Contains(BrickCoord);
		if (bAlreadyPooled || (!bAlreadyComponent && VoxelDebug::GetWaterGpu()))
		{
			ApplyWaterBrickPooled(Impl, ChunkOwner, Material, Impl.PoolSlots, BrickCoord, BrickOriginUU, Quads,
			                      CornerHeights, Activity);
			continue;
		}

		TObjectPtr<UWaterChunkComponent>* ExistingPtr = Impl.ChunkComponents.Find(BrickCoord);
		UWaterChunkComponent* Comp = ExistingPtr ? ExistingPtr->Get() : nullptr;
		if (!Comp)
		{
			// Already inside this tick's kMaxBrickMeshesPerTick budget (the
			// meshBrick<8> call above already happened) -- NewObject +
			// RegisterComponent is comparatively cheap next to that, so no
			// separate cap needed here.
			Comp = NewObject<UWaterChunkComponent>(ChunkOwner);
			Comp->SetupAttachment(ChunkRoot);
			// ChunkRoot sits at the world origin, so the brick's world origin is
			// also its relative location.
			Comp->SetRelativeLocation(BrickOriginUU);
			Comp->SetMaterial(0, Material);
			Comp->RegisterComponent();
			Impl.ChunkComponents.Add(BrickCoord, Comp);
		}
		Comp->SetChunkQuads(MoveTemp(Quads), MoveTemp(CornerHeights), Activity);
	}

	if (DeferredCount > 0)
	{
		UE_LOG(LogVoxelWater, Verbose, TEXT("RemeshDirtyBricks: deferred %d brick mesh(es) to next tick (budget %d/tick)."),
		       DeferredCount, kMaxBrickMeshesPerTick);
	}
}

// --- C7/C8: implicit cavern water ------------------------------------------

// Drains the mobilizer's newly-converted-brick queue. A mobilized brick must
// stop drawing as implicit water and start drawing as CA water in the SAME
// frame or the handover visibly flickers, so this both marks it dirty for the
// CA re-mesh and destroys its implicit component.
void MarkMobilizedBricksDirty(FVoxelWaterImpl& Impl)
{
	for (const vxc::BrickKey& K : Impl.Mob.takeRecentlyMobilized())
	{
		const VoxelCoords::FVoxelCoord C = ToCoord(K);
		Impl.DirtyBricks.Add(C);
		Impl.DirtySinceLastBroadcast.Add(C);
		if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ImplicitChunkComponents.Find(C))
		{
			if (*Existing)
			{
				(*Existing)->DestroyComponent();
			}
			Impl.ImplicitChunkComponents.Remove(C);
		}
		// Same handover under voxel.Water.GPU: the implicit range must stop
		// drawing in the SAME frame the CA's starts, or the two briefly draw the
		// same water and a translucent surface doubled on itself is obvious.
		ReleaseWaterBrickPooled(Impl, Impl.ImplicitPoolSlots, C);
	}
}

// --- THE BASIN LEDGER: host, hand-offs and persistence (Phase 2) ------------
//
// voxelcore/basinledger.h carries the design; what is here is the wiring: build
// the four objects over the fine tier this subsystem already streams, bind the
// datum seam, drain the spill queue into the routing graph, and put the whole
// thing on disk.
//
// WHY IT IS A SEPARATE SAVE FILE. `<seed>.vxhydro`, beside `<seed>.vxwater`
// beside `<seed>.vxlog`. Appending to the water blob was the first design and
// it is not possible: `vxc::WaterState::parse` REFUSES TRAILING BYTES by
// contract (waterca.h), and relaxing that would weaken the one check that turns
// a truncated water save into a refusal instead of a half-applied world.
//
// It is also the better answer on ADR-0005's own argument. The ADR put water in
// its own file rather than in the edit log because it invalidates on
// kWaterCAVersion and is discardable without discarding terrain edits. Hydrology
// invalidates on kRiverNetVersion and kBasinLedgerVersion, which move for
// completely different reasons -- so a third file is the same reasoning applied
// once more, and it means a stale routing-math bump cannot cost a player their
// drained caverns.
//
// SAME ATOMIC WRITE, SAME LOUD FALLBACK. Everything below reuses
// WriteWaterBytesAtomic and mirrors LoadWaterState's three failure modes, so a
// missing or refused hydrology blob reads as "every lake is back at its baked
// equilibrium" and SAYS SO, rather than looking like a world where nothing ever
// happened.

// Builds the ledger stack over the fine tier and binds it to the sampler.
// Idempotent, and a no-op when there is no fine tier: `ribbonTiles()` is the
// IWaterSampler hook that answers null for NullWaterSampler, which is exactly
// the "no baked tiles" configuration.
void EnsureBasinLedger(FVoxelWaterImpl& Impl)
{
	if (Impl.Basins || !Impl.Water)
	{
		return;
	}
	vxc::FineTileSampler* Tiles = Impl.Water->ribbonTiles();
	if (Tiles == nullptr)
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("Basin ledger DISABLED: no fine tier, so there is no basin table to keep a volume ledger of. ")
		       TEXT("Lakes stand at their baked equilibrium, exactly as before Phase 2."));
		return;
	}
	// Built in dependency order; see the declaration comment in FVoxelWaterImpl
	// for why the order is load-bearing on the way out as well as in.
	Impl.BasinTerrain = std::make_unique<vxc::FineTileBasinTerrain>(*Tiles);
	Impl.BasinCapacity = std::make_unique<vxc::ClientHypsometryProvider>(*Impl.BasinTerrain);
	Impl.Basins = std::make_unique<vxc::BasinLedger>(*Impl.BasinCapacity);
	Impl.BasinDatum = std::make_unique<vxc::BasinLedgerDatumSource>(*Impl.Basins);
	Impl.Water->setBasinDatumSource(Impl.BasinDatum.get());

	UE_LOG(LogVoxelWater, Log,
	       TEXT("Basin ledger ENABLED (kBasinLedgerVersion %u): lake datums now read surfaceMm + h(volume delta). ")
	       TEXT("Capacity comes from a CLIENT-RECONSTRUCTED hypsometry (v1 tiles ship none); it is replaced by the ")
	       TEXT("baked curve when basin table v2 lands, with no change above the provider interface."),
	       vxc::kBasinLedgerVersion);
}

// The fine grid stride the ledger needs before it can turn a tile-local outlet
// pixel into world voxels. Not knowable until a tile is resident, so it is
// pushed in lazily rather than at construction -- and refusing to spill until it
// is known beats guessing 8192 and routing a lake's overflow into the wrong
// valley (basinledger.h `setTilePixels`).
void SyncBasinTileStride(FVoxelWaterImpl& Impl)
{
	if (!Impl.BasinCapacity || !Impl.Water)
	{
		return;
	}
	const uint32 Stride = Impl.Water->tilePixels();
	if (Stride != 0 && Impl.BasinCapacity->tilePixels() != Stride)
	{
		Impl.BasinCapacity->setTilePixels(Stride);
	}
}

// Moves `Units` into a basin, keeping every consumer of its height in step.
//
// THE THREE THINGS A CREDIT HAS TO DO, and forgetting any one of them is a
// visible bug rather than an inefficiency: move the scalar, drop the sampler's
// one-entry column memo (or the near field answers the old height for one more
// pixel), and mark the basin for the next replication round.
// A NEGATIVE `Units` DEBITS, and routes to the ledger's own debit() rather than
// to a credit of a negative number, because the two have different bounds:
// credit is bounded above by the sill (the excess spills), debit is bounded
// below by the basin going empty (it cannot invent water by draining past its
// own floor). Returning the magnitude actually moved keeps one contract for
// both directions.
int64 CreditBasinAndSync(FVoxelWaterImpl& Impl, vxc::BasinId Id, int64 Units)
{
	if (!Impl.Basins || Units == 0)
	{
		return 0;
	}
	SyncBasinTileStride(Impl);
	const int64 Moved = Units > 0 ? Impl.Basins->credit(Id, Units) : Impl.Basins->debit(Id, -Units);
	if (Moved > 0)
	{
		Impl.Water->invalidateBasinDatumMemo();
		Impl.BasinDirtySinceLastBroadcast.Add(Id.v);
	}
	return Moved;
}

// Drains the ledger's spill queue into the routing graph, ledgered on both
// sides: whatever the graph will not take comes back to the basin it came from
// and back-pressures the lake, rather than disappearing.
//
// NO GRAPH IS NOT AN ERROR. voxel.Water.Rivers is off by default, so the common
// case is that there is nothing to route into -- every spill event is then
// refunded, the lake sits above its sill, and `BasinSpillUnitsRefunded` says
// so. That is the honest behaviour for a world with no downstream: the water is
// real and there is nowhere for it to go.
void RouteBasinSpills(FVoxelWaterImpl& Impl)
{
	// --- Phase 3 sill-faucet housekeeping, BEFORE the queue drain ----------
	// Held events are water the fluid host owes; two ways they stop being
	// held: the host drains them (DrainFluidSpillFaucets), or this flush
	// refunds them -- on intercept disable, or after a grace window that a
	// live host can never hit (it drains every tick; 10 s is ~600 ticks).
	if (!Impl.FluidSpillHeld.empty())
	{
		constexpr double kFluidSpillGraceSeconds = 10.0;
		const double Now = FPlatformTime::Seconds();
		for (auto It = Impl.FluidSpillHeld.begin(); It != Impl.FluidSpillHeld.end();)
		{
			if (!Impl.bFluidSpillInterceptEnabled ||
			    Now - It->HeldSinceSeconds > kFluidSpillGraceSeconds)
			{
				const int64 Back = Impl.Basins ? Impl.Basins->refundSpill(It->Event.basin, It->Event.units) : 0;
				Impl.FluidSpillUnitsTimedOut += Back;
				Impl.BasinSpillUnitsRefunded += Back;
				if (Back > 0 && Impl.Water)
				{
					Impl.Water->invalidateBasinDatumMemo();
				}
				It = Impl.FluidSpillHeld.erase(It);
			}
			else
			{
				++It;
			}
		}
	}

	if (!Impl.Basins || Impl.Basins->pendingSpill().empty())
	{
		return;
	}
	// One fine pixel of reach. The baked outlet is "the saddle the basin spills
	// over" and the graph's nodes sit on the same 1.875 m lattice, so a spill
	// that cannot find a reach within one pixel is not near its own channel and
	// must refuse rather than search outward into a neighbouring valley.
	const int64 MaxReachMm = Impl.Water ? int64(Impl.Water->pixelSizeMm()) : 1875;

	// An explicit drain rather than vxc::routeSpills, because the Phase 3
	// sill-faucet intercept needs the BASIN ID of each event (to refund what
	// the fluid cannot emit) and routeSpills' inject callback deliberately
	// does not carry it. The graph-or-refund arm below is routeSpills'
	// documented semantics verbatim: clamp what the graph took, refund the
	// remainder to the basin it came from.
	const double Now = FPlatformTime::Seconds();
	int64 Routed = 0;
	int64 Refunded = 0;
	for (const vxc::BasinSpillEvent& E : Impl.Basins->drainSpillEvents())
	{
		// Phase 3 intercept: an outlet inside the fluid's active region is the
		// fluid's to emit as particles (a sill FAUCET), not the graph's to
		// route. Held, timestamped, and owed -- the flush above refunds it if
		// the fluid host never drains it.
		if (Impl.bFluidSpillInterceptEnabled)
		{
			const FVector OutletUU =
				VoxelCoords::VoxelToWorldCenter(VoxelCoords::FVoxelCoord{E.outletVx, E.outletVy, 0});
			if (OutletUU.X >= Impl.FluidSpillBoxMinXUU && OutletUU.X <= Impl.FluidSpillBoxMaxXUU &&
			    OutletUU.Y >= Impl.FluidSpillBoxMinYUU && OutletUU.Y <= Impl.FluidSpillBoxMaxYUU)
			{
				Impl.FluidSpillHeld.push_back({E, Now});
				Impl.FluidSpillUnitsClaimed += E.units;
				continue;
			}
		}

		int64 Took = 0;
		if (Impl.Rivers)
		{
			const uint32 Seg = Impl.Rivers->nearestSegmentToVoxel(E.outletVx, E.outletVy, MaxReachMm);
			if (Seg != vxc::RiverNetwork::kNoSegment)
			{
				const int32 Amount = int32(vxc::clampi64(E.units, 0, INT32_MAX));
				Impl.Rivers->injectInflow(Seg, Amount);
				Took = int64(Amount);
			}
		}
		Routed += Took;
		const int64 Back = E.units - Took;
		if (Back > 0)
		{
			Refunded += Impl.Basins->refundSpill(E.basin, Back);
		}
	}
	Impl.BasinSpillUnitsRouted += Routed;
	Impl.BasinSpillUnitsRefunded += Refunded;
	if (Refunded > 0)
	{
		Impl.Water->invalidateBasinDatumMemo(); // a refund raised a lake back up
	}
}

// --- ADR-0005 water persistence -------------------------------------------
//
// The blob is a SIBLING of the terrain edit log, written into the same
// directory with the same per-seed naming and the same atomic tmp+rename write
// UVoxelWorldSubsystem uses (GetWorldSaveFilePath / WriteBytesAtomic in
// VoxelWorldSubsystem.cpp). Only the extension differs: <seed>.vxwater beside
// <seed>.vxlog. See waterca.h's WaterState comment for why it is a separate
// file rather than a section of the log (it invalidates on kWaterCAVersion and
// is discardable without discarding terrain edits).
FString GetWaterSaveFilePath(uint64 Seed)
{
	return FPaths::ProjectSavedDir() / TEXT("VoxelWorlds") / FString::Printf(TEXT("%llu.vxwater"), (unsigned long long)Seed);
}

// The .vxlog the terrain edit log saves to -- used only to decide how LOUD a
// missing water blob should be (a missing blob beside an existing terrain save
// means a world that had edits, some possibly drains, will refill every cavern;
// a missing blob with no terrain save is just a fresh world).
FString GetTerrainSaveFilePath(uint64 Seed)
{
	return FPaths::ProjectSavedDir() / TEXT("VoxelWorlds") / FString::Printf(TEXT("%llu.vxlog"), (unsigned long long)Seed);
}

// Atomic tmp+rename write, identical shape to VoxelWorldSubsystem.cpp's
// WriteBytesAtomic: a process dying mid-write leaves only the .tmp behind,
// never a truncated/corrupt blob a later load would have to reject.
bool WriteWaterBytesAtomic(const FString& Path, const TArray<uint8>& Bytes)
{
	const FString TmpPath = Path + TEXT(".tmp");
	if (!FFileHelper::SaveArrayToFile(Bytes, *TmpPath))
	{
		UE_LOG(LogVoxelWater, Error, TEXT("SaveWaterState: failed to write temp file %s"), *TmpPath);
		return false;
	}
	if (!IFileManager::Get().Move(*Path, *TmpPath, /*Replace*/ true))
	{
		UE_LOG(LogVoxelWater, Error, TEXT("SaveWaterState: failed to rename %s -> %s"), *TmpPath, *Path);
		IFileManager::Get().Delete(*TmpPath);
		return false;
	}
	return true;
}

bool SaveWaterStateToDisk(const FVoxelWaterImpl& Impl, uint64 Seed)
{
	// vxc::WaterState::serialize appends the whole blob (magic + kFormatVersion +
	// kWaterCAVersion + totalVolume integrity check + per-brick fill + active set
	// + mobilized keys) into a caller-owned buffer, exactly like EditLog.
	std::vector<uint8_t> Bytes;
	vxc::WaterState::serialize(Impl.CA, Impl.Mob, Bytes);

	TArray<uint8> OutBytes;
	OutBytes.SetNumUninitialized((int32)Bytes.size());
	if (!Bytes.empty())
	{
		FMemory::Memcpy(OutBytes.GetData(), Bytes.data(), Bytes.size());
	}

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VoxelWorlds");
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	const FString Path = GetWaterSaveFilePath(Seed);
	const bool bWaterWritten = WriteWaterBytesAtomic(Path, OutBytes);

	// Phase 2's blob goes out from HERE rather than from each call site, so
	// every path that saves water saves hydrology -- the Deinitialize autosave,
	// the console command and any future one -- and none of them can be added
	// later having forgotten it.
	//
	// ATTEMPTED EVEN IF THE WATER BLOB FAILED. The two are independent files
	// with independent version gates (see SaveHydroStateToDisk), and losing the
	// player's lake levels because a different file could not be renamed is not
	// a trade this makes on its own.
	if (!SaveHydroStateToDisk(Impl, Seed))
	{
		UE_LOG(LogVoxelWater, Error,
		       TEXT("SaveWaterState: the water blob %s, but the HYDROLOGY blob failed to write -- lake levels and ")
		       TEXT("the routing graph will revert to baked equilibrium on the next load."),
		       bWaterWritten ? TEXT("was written") : TEXT("also failed"));
	}
	if (!bWaterWritten)
	{
		return false;
	}

	vxc::Digest D;
	Impl.CA.digest(D);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("SaveWaterState: wrote %d bytes (%llu fill units, %llu stored brick(s), %llu mobilized brick(s)) to %s -- waterDigest=0x%016llX"),
	       OutBytes.Num(), (unsigned long long)Impl.CA.totalVolume(), (unsigned long long)Impl.CA.storedBrickCount(),
	       (unsigned long long)Impl.Mob.mobilizedBricks().size(), *Path, (unsigned long long)D.h);
	return true;
}

void LoadWaterStateFromDisk(FVoxelWaterImpl& Impl, uint64 Seed)
{
	// Honor -VoxelNoLoad exactly as LoadEditLogFromDisk does: a fresh-start
	// verification aid must start the water field fully implicit too, or terrain
	// would start fresh while water came back from a stale blob.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelNoLoad")))
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("LoadWaterState: -VoxelNoLoad passed -- skipping water-state load; underground water starts fully implicit."));
		return;
	}

	const FString Path = GetWaterSaveFilePath(Seed);
	if (!FPaths::FileExists(Path))
	{
		// FAILURE MODE 1 -- MISSING. Coherent (the world reverts to the implicit,
		// full field) but every drained cavern refills, so it is LOUD whenever a
		// terrain save exists (that world had edits; some may have been drains)
		// and merely informational for a genuinely fresh world with no log either.
		if (FPaths::FileExists(GetTerrainSaveFilePath(Seed)))
		{
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("LoadWaterState: a terrain save exists but there is NO water blob at %s -- falling back to a fully ")
			       TEXT("implicit world. EVERY drained cavern in this world WILL REFILL (ADR-0005). Expected only for a save ")
			       TEXT("that predates water persistence; otherwise the water blob was lost."),
			       *Path);
		}
		else
		{
			UE_LOG(LogVoxelWater, Log,
			       TEXT("LoadWaterState: no water blob at %s and no terrain save -- fresh world, underground water is fully implicit."),
			       *Path);
		}
		return;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("LoadWaterState: water blob %s exists but could NOT be READ -- falling back to a fully implicit world; ")
		       TEXT("every drained cavern WILL REFILL (ADR-0005)."),
		       *Path);
		return;
	}

	// FAILURE MODES 2 (stale kWaterCAVersion) and 3 (refused: corrupt/truncated,
	// integrity cross-check mismatch, non-fresh target) collapse into the single
	// signal the ADR-0005 handoff names: vxc::WaterState::load returning false.
	// All three take the SAME loud fallback, because handling any of them quietly
	// silently loses every drained lake. load() applies fills FIRST, then the
	// active set, then markMobilized (waterca.h) -- so the world is never even
	// momentarily in the both-accountants-report-zero state that reads as open air.
	if (!vxc::WaterState::load(Bytes.GetData(), (size_t)Bytes.Num(), Impl.CA, Impl.Mob))
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("LoadWaterState: water blob %s (%d bytes) was REFUSED by vxc::WaterState::load -- stale kWaterCAVersion ")
		       TEXT("(engine is now v%u), corrupt/truncated, or a failed totalVolume integrity cross-check. Falling back to a ")
		       TEXT("fully implicit world; EVERY drained cavern in this world WILL REFILL (ADR-0005)."),
		       *Path, Bytes.Num(), vxc::kWaterCAVersion);
		return;
	}

	// Success. Every restored brick needs meshing on the first frame: the
	// mobilized ones must stop drawing as implicit water and start drawing as CA
	// water (drain takeRecentlyMobilized via MarkMobilizedBricksDirty, tearing
	// down any implicit component -- there are none yet this early, so this just
	// queues the CA re-mesh), and every stored CA brick must mesh its fill.
	MarkMobilizedBricksDirty(Impl);
	for (const auto& Entry : Impl.CA.bricks())
	{
		Impl.DirtyBricks.Add(ToCoord(Entry.first));
	}

	vxc::Digest D;
	Impl.CA.digest(D);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("LoadWaterState: restored %llu fill units across %llu stored brick(s), %llu mobilized brick(s) from %s -- waterDigest=0x%016llX"),
	       (unsigned long long)Impl.CA.totalVolume(), (unsigned long long)Impl.CA.storedBrickCount(),
	       (unsigned long long)Impl.Mob.mobilizedBricks().size(), *Path, (unsigned long long)D.h);
}

// --- Phase 2 hydrology persistence: <seed>.vxhydro --------------------------
//
// TWO INDEPENDENT SECTIONS in one length-prefixed container, and the
// independence is the design: a routing blob that will not apply (a graph
// rebuilt over different bounds, a kRiverNetVersion bump) must not cost the
// player their lake levels, and vice versa. Each section carries its own magic
// and its own version inside voxel-core, so this container only has to say how
// long each one is.
//
//   u32 magic, u32 kHydroFormatVersion
//   u32 ledgerBytes, <BasinLedgerState blob>
//   u32 graphBytes,  <RiverNetState blob>   (graphBytes == 0 when unarmed)
//
// The lengths are what let a reader skip a section it cannot use instead of
// losing its place in the stream -- the same job `entry_bytes` does in the
// basin table and for the same reason.
constexpr uint32 kHydroMagic = 0x59485856; // "VXHY" little-endian
constexpr uint32 kHydroFormatVersion = 1;

FString GetHydroSaveFilePath(uint64 Seed)
{
	return FPaths::ProjectSavedDir() / TEXT("VoxelWorlds") / FString::Printf(TEXT("%llu.vxhydro"), (unsigned long long)Seed);
}

bool SaveHydroStateToDisk(const FVoxelWaterImpl& Impl, uint64 Seed)
{
	if (!Impl.Basins)
	{
		return true; // no fine tier: nothing to persist, and not a failure
	}
	std::vector<uint8_t> Ledger;
	vxc::BasinLedgerState::serialize(*Impl.Basins, Ledger);

	std::vector<uint8_t> Graph;
	if (Impl.Rivers)
	{
		vxc::RiverNetState::serialize(*Impl.Rivers, Graph);
	}

	std::vector<uint8_t> Bytes;
	vxc::ByteWriter W(Bytes);
	W.u32(kHydroMagic);
	W.u32(kHydroFormatVersion);
	W.u32(uint32(Ledger.size()));
	// ByteWriter appends through the vector it was handed, so interleaving these
	// bulk inserts with its u32s is exact -- it holds the container, not an
	// iterator that a reallocation could invalidate.
	Bytes.insert(Bytes.end(), Ledger.begin(), Ledger.end());
	W.u32(uint32(Graph.size()));
	Bytes.insert(Bytes.end(), Graph.begin(), Graph.end());

	TArray<uint8> OutBytes;
	OutBytes.SetNumUninitialized((int32)Bytes.size());
	if (!Bytes.empty())
	{
		FMemory::Memcpy(OutBytes.GetData(), Bytes.data(), Bytes.size());
	}

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VoxelWorlds");
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	const FString Path = GetHydroSaveFilePath(Seed);
	if (!WriteWaterBytesAtomic(Path, OutBytes))
	{
		return false;
	}

	vxc::Digest D;
	Impl.Basins->digest(D);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("SaveHydroState: wrote %d bytes to %s -- %llu basin(s) off equilibrium (sum %lld units, %lld spilled, ")
	       TEXT("%lld routed, %lld refunded, %lld replicated rows), %u graph segment(s), %llu graph diff(s). ")
	       TEXT("Capacity provider: %llu curve(s) built over %llu cell(s), %llu unresolved, %llu apron miss(es). ")
	       TEXT("basinDigest=0x%016llX"),
	       OutBytes.Num(), *Path, (unsigned long long)Impl.Basins->basinCount(),
	       (long long)Impl.Basins->sumOfDeltas(), (long long)Impl.Basins->totalSpilled(),
	       (long long)Impl.BasinSpillUnitsRouted, (long long)Impl.BasinSpillUnitsRefunded,
	       (long long)Impl.BasinReplicatedRows,
	       Impl.Rivers ? Impl.Rivers->segmentCount() : 0u,
	       (unsigned long long)(Impl.Rivers ? Impl.Rivers->diffLog().size() : 0u),
	       // THE RAN-FLAGS for the half of Phase 2 that has no other symptom: a
	       // zero here says the curve builder was NEVER ASKED, which is a
	       // different fact from "no lake moved", and `apronMisses` says how
	       // many shorelines were integrated against a 4x4 stencil that reached
	       // a block nobody had fetched.
	       (unsigned long long)(Impl.BasinCapacity ? Impl.BasinCapacity->hypsometryBuilds() : 0ull),
	       (unsigned long long)(Impl.BasinCapacity ? Impl.BasinCapacity->hypsometryCells() : 0ull),
	       (unsigned long long)(Impl.BasinCapacity ? Impl.BasinCapacity->unresolvedBasins() : 0ull),
	       (unsigned long long)(Impl.BasinTerrain ? Impl.BasinTerrain->apronMisses() : 0ull),
	       (unsigned long long)D.h);
	return true;
}

void LoadHydroStateFromDisk(FVoxelWaterImpl& Impl, uint64 Seed)
{
	if (!Impl.Basins)
	{
		return;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelNoLoad")))
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("LoadHydroState: -VoxelNoLoad passed -- skipping; every lake starts at its baked equilibrium."));
		return;
	}
	const FString Path = GetHydroSaveFilePath(Seed);
	if (!FPaths::FileExists(Path))
	{
		// A missing hydrology blob beside an EXISTING water save is the loud
		// case, by the same reasoning ADR-0005 uses: that world was played, its
		// lakes may have risen or been drained, and all of it is now back at the
		// climate's equilibrium.
		if (FPaths::FileExists(GetWaterSaveFilePath(Seed)))
		{
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("LoadHydroState: a water save exists but there is NO hydrology blob at %s -- every lake ")
			       TEXT("reverts to its BAKED EQUILIBRIUM level and the routing graph starts empty. Expected only ")
			       TEXT("for a save that predates Phase 2; otherwise the blob was lost."),
			       *Path);
		}
		return;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("LoadHydroState: %s exists but could NOT be READ -- lakes revert to baked equilibrium."), *Path);
		return;
	}

	vxc::ByteReader R(Bytes.GetData(), size_t(Bytes.Num()));
	uint32_t Magic = 0, Version = 0, LedgerBytes = 0;
	if (!R.u32(Magic) || Magic != kHydroMagic || !R.u32(Version) || Version != kHydroFormatVersion ||
	    !R.u32(LedgerBytes))
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("LoadHydroState: %s (%d bytes) is not a v%u hydrology blob -- REFUSED. Lakes revert to baked ")
		       TEXT("equilibrium."),
		       *Path, Bytes.Num(), kHydroFormatVersion);
		return;
	}
	// The header is 12 bytes; sections follow it in order and their lengths must
	// stay inside the file. Checked before either is read, so a garbage length
	// cannot walk off the end of the buffer.
	const size_t Total = size_t(Bytes.Num());
	if (size_t(LedgerBytes) + 16 > Total)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("LoadHydroState: %s has an out-of-range ledger section -- REFUSED."),
		       *Path);
		return;
	}
	const uint8* LedgerAt = Bytes.GetData() + 12;
	if (LedgerBytes > 0 && !vxc::BasinLedgerState::load(LedgerAt, size_t(LedgerBytes), *Impl.Basins))
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("LoadHydroState: the basin-ledger section of %s was REFUSED by vxc::BasinLedgerState::load ")
		       TEXT("(stale kBasinLedgerVersion -- engine is now v%u -- corrupt, or a failed sum cross-check). ")
		       TEXT("Every lake reverts to its BAKED EQUILIBRIUM level."),
		       *Path, vxc::kBasinLedgerVersion);
		// Fall through: a bad ledger must not also cost the routing graph.
	}

	const uint8* GraphLenAt = LedgerAt + LedgerBytes;
	vxc::ByteReader R2(GraphLenAt, Total - 12 - size_t(LedgerBytes));
	uint32_t GraphBytes = 0;
	if (!R2.u32(GraphBytes) || size_t(GraphBytes) + 16 + size_t(LedgerBytes) > Total)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("LoadHydroState: %s has an out-of-range graph section -- graph not restored."),
		       *Path);
		GraphBytes = 0;
	}
	if (GraphBytes > 0)
	{
		// HELD, NOT APPLIED. There is no graph yet -- see PendingHydroGraphBlob.
		const uint8* GraphAt = GraphLenAt + 4;
		Impl.PendingHydroGraphBlob.assign(GraphAt, GraphAt + GraphBytes);
	}

	Impl.Water->invalidateBasinDatumMemo();
	vxc::Digest D;
	Impl.Basins->digest(D);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("LoadHydroState: restored %llu basin(s) off equilibrium (sum %lld units) from %s; %d graph byte(s) ")
	       TEXT("held for the next voxel.Water.Rivers arm. basinDigest=0x%016llX"),
	       (unsigned long long)Impl.Basins->basinCount(), (long long)Impl.Basins->sumOfDeltas(), *Path,
	       int32(Impl.PendingHydroGraphBlob.size()), (unsigned long long)D.h);
}

// How far around the camera implicit lake surfaces are built, in water bricks
// (8 voxels = 0.8 m each). 32 bricks is ~25 m, comfortably more than a cavern
// room's half-width, so standing in a flooded chamber shows its whole surface.
constexpr int32 kImplicitRadiusBricks = 32;
constexpr int32 kImplicitRadiusBricksZ = 16;
// Same budget discipline as kMaxBrickMeshesPerTick: the candidate box is large
// and a first refresh must never land as one frame's worth of work.
constexpr int32 kMaxImplicitMeshesPerTick = 192;

// Rebuilds the implicit-water candidate list when the camera crosses into a new
// brick, then meshes a budgeted slice of it every tick.
//
// WHY THIS MESHES BRICKS RATHER THAN DROPPING IN A FLAT PLANE. A plane at
// floodZ (the AVoxelOceanActor trick) would be cheaper and the design doc even
// suggests the same material family — but mobilization would then be a visible
// seam, a flat sheet abruptly becoming voxel water. Going through the very same
// meshBrick<8> path the CA uses means an implicit brick and a mobilized brick
// are pixel-identical, so a draining lake reads as one continuous body with a
// hole appearing in it. That is precisely the shot this feature has to sell.
void RefreshImplicitWater(FVoxelWaterImpl& Impl, const FVector& CameraUU, AActor* ChunkOwner,
                          USceneComponent* ChunkRoot, UMaterialInterface* Material)
{
	if (!ChunkOwner || !ChunkRoot)
	{
		return; // dedicated server: never render
	}

	const int64 CamVx = int64(FMath::FloorToDouble(CameraUU.X / VoxelCoords::VoxelSizeUU));
	const int64 CamVy = int64(FMath::FloorToDouble(CameraUU.Y / VoxelCoords::VoxelSizeUU));
	const int64 CamVz = int64(FMath::FloorToDouble(CameraUU.Z / VoxelCoords::VoxelSizeUU));
	const VoxelCoords::FVoxelCoord Center{
		int32(vxc::floorDiv(CamVx, vxc::WaterBrick8::kEdge)), int32(vxc::floorDiv(CamVy, vxc::WaterBrick8::kEdge)),
		int32(vxc::floorDiv(CamVz, vxc::WaterBrick8::kEdge))};

	// THE WINDOW, not the centre, is what the sweep is keyed on now.
	//
	// The candidate predicate below does not mention the camera -- a column is
	// wet or it is not, a brick is under its flood level or it is not, a brick
	// is proven-interior or it is not. The camera contributes only the BOX that
	// clips that predicate. So a camera step does not invalidate the candidate
	// set; it only moves the box, and the bricks in the overlap are provably
	// unchanged and already meshed.
	//
	// That is why this is a DIFFERENCE and not a rebuild, and why the result is
	// bit-identical to the rebuild it replaces rather than an approximation of
	// it. voxelcore/waterwindow.h carries the argument in full and
	// voxel-core/tests/test_waterwindow.cpp pins it;
	// docs/measurements/water-refresh-2026-08-05.txt is what it is worth.
	const vxc::WaterWindow NewWindow =
		vxc::waterWindowAt(Center.X, Center.Y, Center.Z, kImplicitRadiusBricks, kImplicitRadiusBricksZ);

	if (!Impl.bImplicitCenterValid || NewWindow != Impl.LastImplicitWindow)
	{
		const vxc::WaterWindow OldWindow =
			Impl.bImplicitCenterValid ? Impl.LastImplicitWindow : vxc::WaterWindow{};
		Impl.LastImplicitCenterBrick = Center;
		Impl.LastImplicitWindow = NewWindow;
		Impl.bImplicitCenterValid = true;
		// Zeroed HERE, not in the counter block below: the sweep that follows
		// is what increments them.
		Impl.ImplicitBricksSkippedInterior = 0;
		Impl.ImplicitColumnsSwept = 0;
		Impl.ImplicitBricksEvicted = 0;
		Impl.ImplicitBricksBuriedSkipped = 0;

		// ------------------------------------------------------------------
		// 1. EVICT what left the window.
		//
		// THIS IS ALSO A LEAK FIX. Before the window existed, a rebuild simply
		// Reset() the pending list and re-swept the new box; a brick that had
		// left the box was never revisited, so its UWaterChunkComponent was
		// never destroyed. Nothing anywhere pruned by distance. Flying
		// therefore accumulated implicit water components without bound --
		// every 0.8 m step left a one-brick-wide shell of them behind the
		// camera, all still registered and still drawing.
		// ------------------------------------------------------------------
		{
			vxc::WaterWindow Gone[vxc::kWaterWindowMaxRegions];
			const int NumGone = vxc::waterWindowDifference(OldWindow, NewWindow, Gone);
			for (int R = 0; R < NumGone; ++R)
			{
				const vxc::WaterWindow& G = Gone[R];
				for (int64 By = G.y0; By <= G.y1; ++By)
				{
					for (int64 Bx = G.x0; Bx <= G.x1; ++Bx)
					{
						for (int64 Bz = G.z0; Bz <= G.z1; ++Bz)
						{
							const VoxelCoords::FVoxelCoord C{int32(Bx), int32(By), int32(Bz)};
							if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ImplicitChunkComponents.Find(C))
							{
								if (*Existing)
								{
									(*Existing)->DestroyComponent();
								}
								Impl.ImplicitChunkComponents.Remove(C);
								++Impl.ImplicitBricksEvicted;
							}
							ReleaseWaterBrickPooled(Impl, Impl.ImplicitPoolSlots, C);
						}
					}
				}
			}
			// Columns that left the FOOTPRINT stop being cached. A step that
			// only changes altitude leaves the footprint alone and drops none,
			// which is the point: it then sweeps zero columns.
			vxc::WaterWindow GoneCols[vxc::kWaterWindowMaxColumnRegions];
			const int NumGoneCols = vxc::waterWindowColumnDifference(OldWindow, NewWindow, GoneCols);
			for (int R = 0; R < NumGoneCols; ++R)
			{
				const vxc::WaterWindow& G = GoneCols[R];
				for (int64 By = G.y0; By <= G.y1; ++By)
				{
					for (int64 Bx = G.x0; Bx <= G.x1; ++Bx)
					{
						Impl.ImplicitColumns.Remove(FIntPoint(int32(Bx), int32(By)));
					}
				}
			}
		}

		// ------------------------------------------------------------------
		// 2. The per-COLUMN sweep, unchanged in what it computes.
		//
		// Cheap reject, one column query per brick COLUMN rather than 512 per
		// brick: the flood level is a per-site constant across its whole reach
		// disc, so a column that is dry has no flooded brick anywhere in its
		// stack and the entire vertical run is skipped.
		// ------------------------------------------------------------------
		auto EvaluateColumn = [&Impl](int32 Bx, int32 By) -> FVoxelWaterImpl::FImplicitColumn
		{
			FVoxelWaterImpl::FImplicitColumn Col;
			const int64 Vx = int64(Bx) * vxc::WaterBrick8::kEdge;
			const int64 Vy = int64(By) * vxc::WaterBrick8::kEdge;

			// THE CEILING IS THE HIGHER OF THE TWO IMPLICIT WATER SOURCES, and
			// it used to be only the first. This filter read `cavern.floodZMm`
			// alone, so a column with no CAVERN was skipped outright -- and a
			// baked surface lake lives in exactly such a column. Measured
			// before the fix, at a lake with 19.6 m of water standing over the
			// camera's own column: "0 candidate brick(s)".
			//
			// THE OCEAN IS DELIBERATELY *NOT* A THIRD CEILING HERE (work item
			// 8): it is already drawn by AVoxelOceanActor's 40 km plane, and it
			// would be every brick in the box. A MOBILIZED sea brick is
			// unaffected -- the CA's own component map meshes it.
			//
			// FROM THE TERRAIN'S OWN AMPLIFIER: this line is the whole of the
			// 2026-08-04 floating-disc defect. It read `Impl.Amp.columnCached`,
			// this file's private Amplifier over a SyntheticTileSampler, i.e. a
			// DIFFERENT WORLD on any baked run -- which put a flat slab of
			// water 528 m in the air, carried along with the camera.
			Col.CavernZMm = Impl.CavernFloodMmAt(Vx, Vy);
			Col.LakeZMm = Impl.Water->waterSurfaceMmAtVoxel(Vx, Vy);
			if (Col.CavernZMm == INT32_MIN && Col.LakeZMm == vxc::kNoWaterMm)
			{
				return Col; // dry column: no cavern below it and no lake on it
			}

			// THE PADDED FOOTPRINT'S GROUND BOUND. It bounds the cavern term
			// from above (`vxc::implicitWaterCeilingMm`) as well as proving a
			// lake's interior, and both need the same number over the same
			// footprint.
			const int64 Px0 = Vx - 1, Py0 = Vy - 1;
			const int64 Px1 = Vx + vxc::WaterBrick8::kEdge, Py1 = Vy + vxc::WaterBrick8::kEdge;
			Col.GroundUpperMm = Impl.Terrain.GetSurfaceUpperBoundMm(Px0, Py0, Px1, Py1);
			// MIN_int64 is the accessor's "no information" -- never a low
			// bound. MAX_int64 is what "do not bound the cavern term" reads as
			// below. Failing OPEN here, not closed: an unbounded ceiling offers
			// a brick that meshes to nothing, a wrong bound deletes a lake.
			const int64 GroundCeilMm = Col.GroundUpperMm == MIN_int64 ? MAX_int64 : Col.GroundUpperMm;

			const int64 FloodZMm = vxc::implicitWaterCeilingMm(Col.CavernZMm, Col.LakeZMm, GroundCeilMm);
			if (FloodZMm == vxc::kNoImplicitWaterMm)
			{
				return Col; // a cavern in reach, but none of its water is in this column
			}
			Col.FloodBrickZ = vxc::floorDiv(FloodZMm / vxc::kVoxelSizeMm, vxc::WaterBrick8::kEdge);
			Col.bAdmitted = true;

			// THE FLOOR, which this sweep has never had, and which the
			// measurement says is the binding cost rather than the rebuild.
			//
			// The vertical run starts at the BOX FLOOR with no lower bound from
			// the ground at all, so in shallow water most of what it offers is
			// buried rock. Measured on the bv14 braided reach: of 67,600 bricks
			// offered at one camera, 46,475 (68.8%) are provably underground,
			// and only 12.5% of what the drain actually meshed emitted a single
			// quad. The disc could not finish building because seven eighths of
			// its budget went on rock.
			//
			// `surfaceLowerBoundMm` is the exact mirror of the ceiling above --
			// a GUARANTEED lower bound on the amplified ground over the same
			// padded footprint -- so a brick whose padded TOP is at or below it
			// has every padded cell inside rock, fill 0 everywhere, and
			// therefore no face anywhere. That is a PROOF, the only kind of
			// skip allowed here, and it is checked rather than argued:
			// vxc_waterrefreshprobe meshes every brick this rejects and fails
			// on a single non-empty one.
			//
			// CAVERN COLUMNS ARE EXCLUDED. A cavern's water is bounded by rock
			// and room walls, not by the surface, so "below the ground" says
			// nothing about whether its brick has faces -- the whole point of a
			// cavern lake is that it is under the ground.
			//
			// Declining to bound fails OPEN (MIN_int64 rejects nothing), the
			// same direction the ceiling term fails.
			const bool bLakeOnlyColumn = (Col.CavernZMm == INT32_MIN) && (Col.LakeZMm != vxc::kNoWaterMm);
			if (bLakeOnlyColumn)
			{
				Col.GroundFloorMm = Impl.Terrain.GetSurfaceLowerBoundMm(Px0, Py0, Px1, Py1);
			}

			// THE INTERIOR OF A LAKE IS NOT A CANDIDATE, and skipping it is
			// what makes a lake affordable rather than merely correct. A brick
			// whose whole padded neighbourhood is full water emits NO faces.
			// Measured at the 19.6 m lake on tile (-12,-5): 38,025 candidates
			// of which 33,800 -- 89% -- were interior bricks that produced
			// nothing.
			//
			// The datum floor is sampled at the four corners of the padded
			// footprint. The footprint is 10 voxels = 1.0 m and a fine tile
			// pixel is 1.875 m, so it touches at most 2x2 pixels and the four
			// corners hit every one of them -- exhaustive over the pixels, not
			// a sample of them. All four must agree and be wet; a shoreline
			// anywhere in the footprint makes them disagree and the brick stays
			// a candidate.
			//
			// EDITED BRICKS: the bound is pure worldgen and does not see the
			// overlay, but an edit inside the water funnels through
			// NotifyTerrainVoxelsCleared -> mobilizeEditRegion, which hands the
			// brick to the CA and takes it out of the implicit path altogether.
			bool bDatumUniform = false;
			if (bLakeOnlyColumn)
			{
				bDatumUniform = Impl.Water->waterSurfaceMmAtVoxel(Px0, Py0) == Col.LakeZMm &&
				                Impl.Water->waterSurfaceMmAtVoxel(Px1, Py0) == Col.LakeZMm &&
				                Impl.Water->waterSurfaceMmAtVoxel(Px0, Py1) == Col.LakeZMm &&
				                Impl.Water->waterSurfaceMmAtVoxel(Px1, Py1) == Col.LakeZMm;
			}
			Col.bCanSkipInterior = bLakeOnlyColumn && bDatumUniform && Col.GroundUpperMm != MIN_int64;
			return Col;
		};

		// ------------------------------------------------------------------
		// 3. SWEEP only what entered.
		// ------------------------------------------------------------------
		const int32 PendingBefore = Impl.PendingImplicitBricks.Num();
		vxc::WaterWindow Fresh[vxc::kWaterWindowMaxRegions];
		const int NumFresh = vxc::waterWindowDifference(NewWindow, OldWindow, Fresh);
		for (int R = 0; R < NumFresh; ++R)
		{
			const vxc::WaterWindow& G = Fresh[R];
			for (int64 By64 = G.y0; By64 <= G.y1; ++By64)
			{
				for (int64 Bx64 = G.x0; Bx64 <= G.x1; ++Bx64)
				{
					const int32 Bx = int32(Bx64), By = int32(By64);
					const FIntPoint ColKey(Bx, By);
					FVoxelWaterImpl::FImplicitColumn* Col = Impl.ImplicitColumns.Find(ColKey);
					if (!Col)
					{
						Col = &Impl.ImplicitColumns.Add(ColKey, EvaluateColumn(Bx, By));
						++Impl.ImplicitColumnsSwept;
					}
					if (!Col->bAdmitted)
					{
						continue;
					}
					for (int64 Bz64 = G.z0; Bz64 <= G.z1; ++Bz64)
					{
						const int32 Bz = int32(Bz64);
						// The padded z span this brick's mesh actually reads:
						// voxel bottoms from (Bz*8 - 1) to (Bz*8 + 8).
						const int64 PadBottomMm = (int64(Bz) * vxc::WaterBrick8::kEdge - 1) * vxc::kVoxelSizeMm;
						const int64 PadTopMm =
							(int64(Bz) * vxc::WaterBrick8::kEdge + vxc::WaterBrick8::kEdge) * vxc::kVoxelSizeMm;
						if (Col->bCanSkipInterior)
						{
							// Every padded cell above the ground (open air) AND
							// every padded cell's bottom a full voxel below the
							// datum (fill 255, no partial top): uniform 255 over
							// the whole pad, so no face anywhere in the brick.
							if (PadBottomMm >= Col->GroundUpperMm &&
							    PadTopMm + vxc::kVoxelSizeMm <= int64(Col->LakeZMm))
							{
								++Impl.ImplicitBricksSkippedInterior;
								continue;
							}
						}
						if (Col->GroundFloorMm != MIN_int64 && PadTopMm <= Col->GroundFloorMm)
						{
							++Impl.ImplicitBricksBuriedSkipped;
							continue;
						}
						if (int64(Bz) > Col->FloodBrickZ)
						{
							continue;
						}
						Impl.PendingImplicitBricks.Add(VoxelCoords::FVoxelCoord{Bx, By, Bz});
					}
				}
			}
		}

		// Verbose, not Log: a window step happens up to ~37 times a second at
		// flight speed. The old per-REBUILD line was already firing that often
		// and was tolerable only because a rebuild was rare enough to be
		// interesting; a step is not. The judgeable moment -- "the disc is
		// complete" -- is the DRAINED line below, and that stays at Log.
		UE_LOG(LogVoxelWater, Verbose,
		       TEXT("RefreshImplicitWater: window step to brick (%d,%d,%d) [cam (%.0f,%.0f,%.0f) UU] -- ")
		       TEXT("%d column(s) swept, %d brick(s) added, %d evicted, %d proven-interior, %d proven-buried; ")
		       TEXT("%d pending"),
		       Center.X, Center.Y, Center.Z, CameraUU.X, CameraUU.Y, CameraUU.Z, Impl.ImplicitColumnsSwept,
		       Impl.PendingImplicitBricks.Num() - PendingBefore, Impl.ImplicitBricksEvicted,
		       Impl.ImplicitBricksSkippedInterior, Impl.ImplicitBricksBuriedSkipped,
		       Impl.PendingImplicitBricks.Num());

		// DRAIN ACCOUNTING, and it exists because the absence of it cost a
		// whole diagnosis. A baked lake rendered as scattered bricks, and the
		// candidate count -- the only number this function reported -- looked
		// healthy (42,250) in exactly the run where the sheet was broken,
		// because the defect was that the list never DRAINED.
		//
		// NOTE FOR CAPTURES: tools/voxel-capture.ps1's settle check reads the
		// TERRAIN streaming counters and knows nothing about this queue. A
		// frame can be fully terrain-settled with the water disc still
		// half-built.
		//
		// The serial no longer advances per REBUILD, because there is no
		// rebuild -- it advances per window step, and the drain counters
		// accumulate across steps rather than resetting. Resetting them here
		// would report the last 0.8 m step's cost as the whole disc's.
		Impl.ImplicitRefreshSerial++;
		Impl.ImplicitCandidatesAtRebuild = Impl.PendingImplicitBricks.Num();
		// Armed only when the queue goes from EMPTY to non-empty, i.e. once per
		// drain EPISODE. Re-arming it on every window step would fire the
		// DRAINED line ~13 times a second while flying, since an incremental
		// step's handful of bricks drains inside one tick -- turning the one
		// line that says "the disc is complete, this frame can be judged" into
		// the noisiest thing in the log.
		if (PendingBefore == 0 && Impl.PendingImplicitBricks.Num() > 0)
		{
			Impl.bImplicitDrainReported = false;
		}

		// Farthest first WITHIN THE NEW BATCH, because the drain below Pop()s
		// from the BACK. Only the newly added range is sorted: the rest of the
		// queue is already ordered and re-sorting it every 0.8 m step would put
		// back an O(n log n) per-step cost of exactly the kind this change
		// removes.
		if (Impl.PendingImplicitBricks.Num() > PendingBefore)
		{
			VoxelCoords::FVoxelCoord* Begin = Impl.PendingImplicitBricks.GetData() + PendingBefore;
			VoxelCoords::FVoxelCoord* End = Impl.PendingImplicitBricks.GetData() + Impl.PendingImplicitBricks.Num();
			std::sort(Begin, End,
			          [Center](const VoxelCoords::FVoxelCoord& A, const VoxelCoords::FVoxelCoord& B)
			          {
				          const int64 Da = int64(A.X - Center.X) * (A.X - Center.X) +
				                           int64(A.Y - Center.Y) * (A.Y - Center.Y) +
				                           int64(A.Z - Center.Z) * (A.Z - Center.Z);
				          const int64 Db = int64(B.X - Center.X) * (B.X - Center.X) +
				                           int64(B.Y - Center.Y) * (B.Y - Center.Y) +
				                           int64(B.Z - Center.Z) * (B.Z - Center.Z);
				          return Da > Db;
			          });
		}
	}

	int32 MeshesThisTick = 0;
	int32 Built = 0;
	const double DrainT0 = FPlatformTime::Seconds();
	while (Impl.PendingImplicitBricks.Num() > 0 && MeshesThisTick < kMaxImplicitMeshesPerTick)
	{
		const VoxelCoords::FVoxelCoord BrickCoord = Impl.PendingImplicitBricks.Pop(EAllowShrinking::No);
		// A QUEUED BRICK CAN OUTLIVE THE WINDOW THAT QUEUED IT, and meshing it
		// would put water back outside the disc after the eviction pass above
		// had just destroyed its component.
		//
		// The queue is no longer Reset() on a camera step -- that reset is the
		// thing this change removes -- so entries survive across steps and a
		// slow drain can still be holding bricks the camera has since left
		// behind. Testing containment at POP is what keeps the queue consistent
		// without an O(n) filter on every step: the check is three comparisons
		// and the work it skips is a whole mesh.
		if (!Impl.LastImplicitWindow.contains(BrickCoord.X, BrickCoord.Y, BrickCoord.Z))
		{
			continue;
		}
		const vxc::BrickKey Key = ToBrickKey(BrickCoord);
		const int64_t Ox = int64_t(Key.x) * vxc::WaterBrick8::kEdge;
		const int64_t Oy = int64_t(Key.y) * vxc::WaterBrick8::kEdge;
		const int64_t Oz = int64_t(Key.z) * vxc::WaterBrick8::kEdge;

		++MeshesThisTick;

		// implicitFillAt() already returns 0 for a mobilized brick, so a
		// converted brick simply meshes to nothing here and the CA's own
		// component takes over — the ownership partition does the bookkeeping.
		std::vector<vxc::Quad> RawQuads;
		// Carries the fill fraction as the material id, exactly like the CA
		// sampler in RemeshDirtyBricks -- see the long comment there for what
		// the three uses of that byte are.
		//
		// THIS ONE IS NOT OPTIONAL, and it is not merely for consistency. The
		// byte now drives M_WaterVoxel's World Position Offset via
		// VertexColor.R, so returning the old fixed MAT_ROCK placeholder (= 2)
		// would hand the material R = 2/255 and push the top face of every
		// implicit cavern lake down by ~99% of a voxel -- collapsing every
		// static underground lake in the world to a sub-millimetre film. The
		// implicit field is a static flood level, so in practice this returns
		// 255 below floodZ and 0 above it, which the material reads as full
		// height. Same kMinVisibleFill floor for the same reason.
		//
		// Padded-block-first, for the same two reasons the CA path is (see the
		// FWaterFillPad comment): identical read count, and the corner field below
		// then costs no further implicitFillAt calls.
		FWaterFillPad Pad;
		BuildWaterFillPad(Pad, [&Impl, Ox, Oy, Oz](int32 x, int32 y, int32 z) -> uint8
		{
			return Impl.Mob.implicitFillAt(Ox + x, Oy + y, Oz + z);
		});

		const auto Sampler = [&Pad](int x, int y, int z) -> vxc::MaterialId
		{
			const uint8 Fill = Pad.At(x, y, z);
			return Fill >= kMinVisibleFill ? QuantiseFillForMesh(Fill) : vxc::MAT_AIR;
		};
		vxc::meshBrick<vxc::WaterBrick8::kEdge>(Sampler, RawQuads);

		if (RawQuads.empty())
		{
			// Dry, fully submerged (interior bricks emit no faces — which is
			// exactly what keeps a large lake affordable), or mobilized.
			if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ImplicitChunkComponents.Find(BrickCoord))
			{
				if (*Existing)
				{
					(*Existing)->DestroyComponent();
				}
				Impl.ImplicitChunkComponents.Remove(BrickCoord);
			}
			ReleaseWaterBrickPooled(Impl, Impl.ImplicitPoolSlots, BrickCoord);
			++Impl.ImplicitBricksEmpty;
			continue;
		}
		++Impl.ImplicitBricksMeshed;

		// Same corner-height treatment as the CA path, and it costs this path very
		// little: the implicit field is a per-site FLOOD LEVEL, so within one
		// cavern's reach it is 255 below floodZ and 0 above it. Every lattice
		// corner of a submerged layer then averages to 255 ("full to this cell's
		// ceiling") and a flat lake's top layer agrees corner for corner, so
		// nothing is non-planar and nothing splits -- an implicit lake meshes to
		// the quad count it did before. Where two sites' flood levels meet, or
		// where a lake laps a cavern wall, the same rule applies as anywhere else
		// and a few quads split; that is the point rather than a cost.
		//
		// AND IT IS WHAT KEEPS THE MOBILIZATION HANDOVER INVISIBLE. C8 exists so a
		// brick converting from implicit to CA is pixel-identical across the swap
		// (see this function's own header comment). The moment CA water grew a
		// bilinear surface, an implicit neighbour still drawing flat tops would
		// have put a step exactly on the boundary the handover is meant to hide.
		// Routing both through the same corner field is not consistency for its
		// own sake here -- it is the feature.
		FWaterCornerField CornerField;
		BuildWaterCornerField(Pad, CornerField);

		TArray<FVoxelChunkQuad> Quads;
		TArray<uint32> CornerHeights;
		EmitWaterQuads(RawQuads, CornerField, Quads, CornerHeights);

		const FVector BrickOriginUU(double(Ox) * VoxelCoords::VoxelSizeUU, double(Oy) * VoxelCoords::VoxelSizeUU,
		                            double(Oz) * VoxelCoords::VoxelSizeUU);

		// Same "dispatch on what the brick holds" rule as the CA path above.
		//
		// ACTIVITY IS ZERO HERE AND ALWAYS WILL BE, and that is a statement about
		// what implicit water IS rather than a placeholder: the implicit field is
		// a static worldgen flood level, so no implicit brick is ever in the CA's
		// active set. A brick that starts moving is MOBILIZED, which hands it to
		// the CA half and destroys the implicit range in the same frame
		// (MarkMobilizedBricksDirty) -- so foam appears exactly at the handover
		// and never on a still cavern lake.
		const bool bAlreadyPooled = Impl.ImplicitPoolSlots.Contains(BrickCoord);
		const bool bAlreadyComponent = Impl.ImplicitChunkComponents.Contains(BrickCoord);
		if (bAlreadyPooled || (!bAlreadyComponent && VoxelDebug::GetWaterGpu()))
		{
			ApplyWaterBrickPooled(Impl, ChunkOwner, Material, Impl.ImplicitPoolSlots, BrickCoord, BrickOriginUU, Quads,
			                      CornerHeights, /*Activity=*/0.0f);
			++Built;
			continue;
		}

		TObjectPtr<UWaterChunkComponent>* ExistingPtr = Impl.ImplicitChunkComponents.Find(BrickCoord);
		UWaterChunkComponent* Comp = ExistingPtr ? ExistingPtr->Get() : nullptr;
		if (!Comp)
		{
			Comp = NewObject<UWaterChunkComponent>(ChunkOwner);
			Comp->SetupAttachment(ChunkRoot);
			Comp->SetRelativeLocation(BrickOriginUU);
			Comp->SetMaterial(0, Material);
			Comp->RegisterComponent();
			Impl.ImplicitChunkComponents.Add(BrickCoord, Comp);
		}
		Comp->SetChunkQuads(MoveTemp(Quads), MoveTemp(CornerHeights), /*Activity=*/0.0f);
		++Built;
	}

	if (MeshesThisTick > 0)
	{
		Impl.ImplicitDrainTicks++;
		Impl.ImplicitDrainMs += (FPlatformTime::Seconds() - DrainT0) * 1000.0;
	}

	// ONE Log line per refresh, at the moment the disc is complete -- which is
	// the only moment at which "the lake is a sheet" is a claim the frame can
	// support. Log rather than Verbose on purpose: a capture is judged from
	// this line, and `us/brick` is what makes a regression in the memo chain
	// (see BuildWaterFillPad) visible as a number instead of as a patchy shot.
	if (MeshesThisTick > 0 && Impl.PendingImplicitBricks.Num() == 0 && !Impl.bImplicitDrainReported)
	{
		Impl.bImplicitDrainReported = true;
		const int32 Total = Impl.ImplicitBricksMeshed + Impl.ImplicitBricksEmpty;
		UE_LOG(LogVoxelWater, Log,
		       TEXT("RefreshImplicitWater: DRAINED refresh #%llu -- %d candidate(s) in %d tick(s), %.1f ms total ")
		       TEXT("(%.1f us/brick, %.1f ms/tick); %d brick(s) meshed, %d empty, %d skipped as proven interior; ")
		       TEXT("%d implicit component(s) live."),
		       (unsigned long long)Impl.ImplicitRefreshSerial, Impl.ImplicitCandidatesAtRebuild,
		       Impl.ImplicitDrainTicks, Impl.ImplicitDrainMs,
		       Total > 0 ? (Impl.ImplicitDrainMs * 1000.0) / double(Total) : 0.0,
		       Impl.ImplicitDrainTicks > 0 ? Impl.ImplicitDrainMs / double(Impl.ImplicitDrainTicks) : 0.0,
		       Impl.ImplicitBricksMeshed, Impl.ImplicitBricksEmpty, Impl.ImplicitBricksSkippedInterior,
		       Impl.ImplicitChunkComponents.Num());
	}
	// UNDRAINED is the failure this instrumentation exists for, and it is
	// silent by construction otherwise: the queue simply stays long while the
	// frame looks fine. Report it periodically rather than once, because how
	// FAR it got per tick is the diagnosis.
	else if (MeshesThisTick > 0 && Impl.PendingImplicitBricks.Num() > 0 && (Impl.ImplicitDrainTicks % 60) == 0)
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("RefreshImplicitWater: refresh #%llu STILL DRAINING -- %d of %d candidate(s) left after %d tick(s), ")
		       TEXT("%.1f ms so far (%.1f ms/tick); %d meshed, %d empty."),
		       (unsigned long long)Impl.ImplicitRefreshSerial, Impl.PendingImplicitBricks.Num(),
		       Impl.ImplicitCandidatesAtRebuild, Impl.ImplicitDrainTicks, Impl.ImplicitDrainMs,
		       Impl.ImplicitDrainMs / double(FMath::Max(Impl.ImplicitDrainTicks, 1)),
		       Impl.ImplicitBricksMeshed, Impl.ImplicitBricksEmpty);
	}
	(void)Built;
}

// ===========================================================================
// W4 SHALLOW WATER -- voxel.Water.SWE (docs/adr/0004-swe-fixed-point-coupling.md)
// ===========================================================================
//
// WHAT THIS IS FOR, IN ONE SENTENCE FROM THE ADR: "today a lake is a level, not
// a body of water. It has no current, no waves, no direction." waterca.h's
// Phase C is explicit that it "only ever computes a static equilibrium level,
// instantly-ish over a few ticks, never a surge", and that flow momentum is NOT
// MODELLED. voxelcore/swe.h is the layer that has the momentum -- the face
// fluxes ARE the force field -- and it has been sitting in the tree complete,
// tested and golden-pinned, with `SweCoupleConfig::enabled` defaulting false
// and nothing anywhere constructing an SweGrid. This block is the wiring that
// lets a standalone session turn it on.
//
// WHY THIS IS ALLOWED TO EXIST AT ALL, given ADR-0004 item 3 says DEFER.
// The ADR's deferral is argued entirely from replication: the coupler "is a
// second simulation whose state (membership, dwell counters, sheet depth) must
// replicate or be derived identically on every client, exactly like WaterCA ...
// it is not yet wired into the replication path at all. Enabling it before that
// is a guaranteed desync." Every clause of that is about a peer that could
// disagree. In NM_Standalone there is no mirror and no wire -- the subsystem
// does not even broadcast (Tick's BroadcastWaterDiffs is skipped on
// NM_Standalone) -- so the failure the deferral names cannot occur. MaybeArmSwe
// below therefore REFUSES every other net mode outright and says why. That is
// keeping the ADR's deferral, not overriding it: the moment a client exists,
// this feature is off, and it stays off until someone lands the replication
// plumbing the ADR asks for. Plumbing first, flag second, as separate commits.
//
// WHAT IS DELIBERATELY NOT HERE, AND IT IS THE ONE THING A LOOKER WILL NOTICE
// FIRST: THE RENDERER. ADR-0004's own enablement checklist ends with "the
// visible water surface becomes the UNION of sheet depth and CA fill. A
// renderer drawing only one of them will show water vanishing at a boundary."
// That is exactly what happens today: RemeshDirtyBricks samples the CA, and
// RefreshImplicitWater samples the mobilizer's implicit field, and neither
// knows the sheet exists. So a promoted column's water is simulated, conserved,
// ledgered and INVISIBLE. This is called out in the arming log line, in the
// cvar help text, and in the 1Hz status line, because a tester who pours water
// with this flag on and watches it disappear must be able to tell "correctly
// promoted, not yet drawn" from "destroyed". The meshing/sampler region of this
// file is owned elsewhere and is being rewritten concurrently; the precise
// query the renderer needs is specified in the handoff notes rather than
// guessed at here.

// Columns on a side of the single dense sheet.
//
// ONE dense rectangle, not a tiled residency scheme, because swe.h says the
// dense grid IS the reference core and "a production sheet tiles this; the tick
// rules do not change" -- so tiling is a separable follow-up, and doing it now
// would mean debugging residency and first-light wiring in the same change.
//
// 128 is the footprint ADR-0004 measured: 16,384 columns at 0.364 ms/tick,
// scaling linearly in columns (0.083 ms at 64x64, 1.48 ms at 256x256). At 10Hz
// that is ~3.6 ms/s of game-thread time, well inside this subsystem's <2 ms per
// FRAME budget, and it is paid only while armed. 12.8 m on a side at
// kVoxelSizeMm = 100 covers a poured test basin with room around it; 256 would
// cover more and cost 4x, which is a decision for after the renderer exists.
constexpr int32 kSweSheetColumns = 128;

// Bed seating window, in voxels, relative to the terrain's reported surface
// height for each column. Start a little ABOVE the surface (placed blocks, and
// the surface-height query's own quantisation, can both sit above it) and scan
// down far enough to find the floor of a dug basin.
//
// This window is a COST bound, not a correctness one: a column whose search
// finds no solid voxel simply keeps a non-solid bed, which fails the coupler's
// eligibility predicate (`solidAt(vx, vy, bed)` is its first test) forever, so
// the column stays CA-owned and the CA behaves exactly as it does today. The
// failure mode of too small a window is therefore "less sheet", never "wrong
// sheet". 64 voxels is 6.4 m of downward search.
constexpr int32 kSweBedScanAboveVoxels = 8;
constexpr int32 kSweBedScanVoxels = 64;

// Voxel z of the topmost solid voxel in this column's search window.
//
// Uses the MOBILIZER-WRAPPED solidity function, not the bare terrain query, for
// the same reason the CA is given the wrapped one (waterca.h, "SO WE MAKE IT
// STRUCTURALLY IMPOSSIBLE"): an unmobilized implicit cavern lake reads as SOLID
// through the wrapper. Seating the bed with the bare query would put the
// ownership boundary UNDER water the implicit field still owns, and the
// coupler's eligibility predicate -- which only asks whether the bed is solid
// and whether there is clearance above it -- would then happily promote a
// column over an implicit lake and start moving units the implicit accountant
// still believes it holds. That is the double-occupancy bug waterca.h spends a
// page making structurally impossible, and it would be reintroduced by using
// the wrong callback here. Wrapped everywhere, or nowhere.
int32 SeatSweBedZ(const vxc::WaterCA::SolidFn& Solid, int64 Vx, int64 Vy, int64 SurfaceVz, bool& bOutFound)
{
	const int64 TopZ = SurfaceVz + kSweBedScanAboveVoxels;
	for (int32 K = 0; K < kSweBedScanVoxels + kSweBedScanAboveVoxels; ++K)
	{
		const int64 Z = TopZ - K;
		if (Solid(Vx, Vy, Z) != vxc::MAT_AIR)
		{
			bOutFound = true;
			return int32(Z);
		}
	}
	bOutFound = false;
	return int32(TopZ - (kSweBedScanVoxels + kSweBedScanAboveVoxels));
}

// Re-seats the bed of every sheet column a terrain edit has left resting on a
// voxel that is no longer there. Returns how many beds moved this call.
//
// THE OTHER HALF OF swe.h S5(a)'s CONTRACT, and the bug this exists to fix.
//
// The sheet's beds are seated exactly ONCE, by MaybeArmSwe, and swe.h S5(a)
// deliberately never moves one: "The bed is deliberately NOT re-seated downward
// to follow the hole. Doing so was the first design and it is WRONG: it would
// move the ownership boundary down over voxels the CA is at that moment
// carrying water through." That is correct, and it is correct about the case it
// is written for -- a hole dug in the FLOOR of an SWE lake, where the column
// still holds sheet depth and the metered inrush is the whole design.
//
// It is silent about what happens AFTERWARDS, and nothing enforced the rest.
// Once S5(a) has metered the column out and demoteDwellTicks has elapsed, the
// column is CA-owned with a bed pointing at air -- and SweCaCoupler::eligible()
// tests solidAt(bed) FIRST. So it can never promote again, for the rest of the
// session, no matter what the ground under it actually looks like. Every
// CA-owned column is also an INACTIVE HARD WALL in the sheet numerics
// (swe.h setColumnActive), so what a carve leaves behind is not a gap in the
// sheet: it is a permanent wall exactly where the ground was removed.
//
// MEASURED, on 2026-07-29, by -VoxelSweBreachTest=25 -VoxelSweBreachSwe=1: a
// notch cut through a full basin's rim, from three voxels below the waterline
// up past the rim, produced punctured=0, basinDrawdown=0, frontArrived=0 and
// ca=8 -- a breach that did nothing at all, with every conservation check
// green, because the ~50 columns the carve touched were from that moment
// structurally incapable of ever being punctured, promoted, or flowed through.
// The fixture's own post-breach line calls those stale beds CORRECT, citing
// S5(a). Reproduced in voxel-core at the fixture's exact geometry, and pinned
// by swe_coupler_a_carved_bed_is_lost_to_the_sheet_until_the_caller_reseats_it.
//
// THE RULE, and why this shape:
//
//   * ONLY CA-OWNED COLUMNS ARE RE-SEATED. A column the sheet still owns is
//     either being metered into the CA by S5(a) right now or is inside its
//     demote dwell, and S5(a)'s objection applies to it in full. Deferring is
//     free: eligible() rejects a dead bed, so a punctured column can only ever
//     demote, never re-promote, and it is therefore guaranteed to reach this
//     pass -- at most demoteDwellTicks later. This ordering is why the metered
//     inrush is completely unchanged by this fix.
//   * MOVING THE BED OF A CA-OWNED COLUMN TRANSFERS NOTHING. The column holds
//     no sheet depth (demote is all-or-nothing) and is walled out of the sheet,
//     so this pass cannot strand a fill unit, cannot put a cell in two domains,
//     and cannot move the ADR-0004 ledger. It is run INSIDE the coupled window
//     anyway, so if that ever stops being true the per-tick invariant says so
//     on the very next line instead of a lake going quietly shallow.
//   * THE COUPLER'S OWN HYSTERESIS TAKES IT FROM THERE. A re-seated column
//     promotes promoteDwellTicks (8 ticks = 0.8 s) later if it is eligible at
//     its NEW bed, absorbing whatever CA fill is sitting over it through the
//     ordinary ledgered channel -- exactly the self-healing FlushSweIntoCA
//     already relies on after a save.
//
// WHAT THIS DOES NOT FIX, stated here because the fixture will still say so:
// re-seating lets a breached column rejoin the sheet, but it does not by itself
// empty a basin. swe.h S5 has no SWE->CA channel at a LATERAL boundary -- an
// SWE-owned pool cannot spill into a CA-owned neighbour however much lower that
// neighbour's bed is -- and the floor of a breach notch is a narrow channel,
// which S5's minOpenNeighbours excludes from the sheet on purpose. Closing that
// needs a fourth exchange channel in swe.h S5, which is a kSweVersion bump and
// a re-pin of the SWE golden, and it is NOT done here. See the commit message.
int32 ReseatEditedSweBeds(FVoxelWaterImpl& Impl)
{
	if (!Impl.SweSheet || !Impl.SweCoupler || Impl.SwePendingBedReseat.Num() == 0)
	{
		return 0;
	}

	vxc::SweGrid& Grid = *Impl.SweSheet;
	// THE SAME wrapped callback the arm path seated with and the coupler tests
	// against, for the reason SeatSweBedZ's comment gives: a bed seated through
	// the bare terrain query would sit under water the implicit field still owns.
	const vxc::WaterCA::SolidFn Solid = Impl.Mob.makeSolidFn();
	const int32 SizeX = Grid.sizeX();

	TArray<int32> Pending = Impl.SwePendingBedReseat.Array();
	Pending.Sort();
	Impl.SwePendingBedReseat.Reset();

	int32 Reseated = 0;
	for (const int32 ColumnIndex : Pending)
	{
		const int64 Vx = Grid.originVx() + int64(ColumnIndex % SizeX);
		const int64 Vy = Grid.originVy() + int64(ColumnIndex / SizeX);

		if (Solid(Vx, Vy, Grid.bedAt(Vx, Vy)) != vxc::MAT_AIR)
		{
			continue; // the bed is real (re-filled, or never went): nothing owed
		}
		if (Impl.SweCoupler->isSweColumn(Vx, Vy))
		{
			// swe.h S5(a) is metering this column right now. Requeue; it is
			// guaranteed to be CA-owned within demoteDwellTicks.
			Impl.SwePendingBedReseat.Add(ColumnIndex);
			continue;
		}

		const double SurfaceZUU = Impl.Terrain.GetSurfaceHeightUU(double(Vx) * VoxelCoords::VoxelSizeUU,
		                                                          double(Vy) * VoxelCoords::VoxelSizeUU);
		const int64 SurfaceVz = int64(FMath::FloorToDouble(SurfaceZUU / VoxelCoords::VoxelSizeUU));
		bool bFound = false;
		// bFound == false is handled exactly as the arm loop handles it: the
		// column keeps a non-solid bed, fails eligibility forever, and stays
		// CA-owned. "Less sheet", never "wrong sheet".
		const int32 NewBed = SeatSweBedZ(Solid, Vx, Vy, SurfaceVz, bFound);
		if (NewBed == Grid.bedAt(Vx, Vy))
		{
			continue;
		}
		Grid.setBed(Vx, Vy, NewBed);
		++Reseated;
	}

	Impl.SweBedsReseated += Reseated;
	return Reseated;
}

// Flushes every SWE-owned column's sheet depth back into the CA through the
// coupler's ledgered demotion channel, leaving the grid empty and every column
// CA-owned.
//
// THIS IS WHAT MAKES PERSISTENCE A NO-OP RATHER THAN A FORMAT CHANGE, and it is
// the single most destructive thing to get wrong in this whole change. Sheet
// depth is REAL VOLUME -- swe.h §2: depth is a column total in the same fill
// units the CA uses, 255 == one voxel -- and vxc::WaterState::serialize walks
// the CA's bricks and the mobilizer's key set and knows nothing about a grid.
// So serialising while the sheet holds water writes a blob that is quietly
// short by exactly the sheet's contents, and the loss is invisible until
// someone reloads and finds their lake shallower. Flushing first means the
// units are back in CA cells before the serializer looks, ADR-0005's blob
// format is untouched, kWaterCAVersion is untouched, and a save written with
// this cvar armed is byte-comparable with one written without it.
//
// It goes through forceDemote rather than draining the grid by hand precisely
// because forceDemote runs the REAL transfer channel: demote() pre-flights the
// CA's available capacity, moves the whole column or not one unit of it (a
// half-demoted column would hold sheet depth AND CA fill in the same z-range,
// which swe.h §5's partition forbids), and books the move into the coupler's
// toCA_ ledger. A hand-rolled drain would have to re-derive all of that and
// would be the obvious place for this change to lose water.
//
// ALL-OR-NOTHING MEANS THIS CAN LEGITIMATELY FAIL, on a column whose CA cells
// have no room this instant. There is nothing useful to retry against without
// ticking the CA, so a residue is reported LOUDLY rather than papered over: the
// water is still conserved (it is in the grid, and the ledger still balances),
// it is simply not in the blob, which is precisely the case that must never be
// discovered later from a screenshot.
// Dirty every render brick sitting over a column whose sheet depth moved.
//
// The CA's own changed-brick set cannot cover this: sheet water is not in the
// CA, so a promoted column can change depth every tick while the CA reports
// nothing dirty and the surface freezes at whatever it looked like when the
// water left. This is the sheet's equivalent of that signal.
//
// A depth change dirties the whole z-span the sheet can occupy for that column
// ([bed+1, bed+sheetScanVoxels]) rather than trying to work out which single
// layer moved -- the span is at most sheetScanVoxels tall (8 voxels, so one or
// two bricks), and getting it wrong in the cheap direction means a stale
// surface, which is the failure this function exists to prevent.
void MarkSweDepthChangesDirty(FVoxelWaterImpl& Impl)
{
	if (!Impl.SweSheet || !Impl.SweCoupler)
	{
		return;
	}

	const vxc::SweGrid& Grid = *Impl.SweSheet;
	const int32 Cols = kSweSheetColumns;
	const int32 ScanVoxels = Impl.SweCoupler->config().sheetScanVoxels;

	if (Impl.SweLastDepth.size() != size_t(Cols) * size_t(Cols))
	{
		Impl.SweLastDepth.assign(size_t(Cols) * size_t(Cols), 0);
	}

	const int64 OriginVx = Grid.originVx();
	const int64 OriginVy = Grid.originVy();

	for (int32 Cy = 0; Cy < Cols; ++Cy)
	{
		for (int32 Cx = 0; Cx < Cols; ++Cx)
		{
			const int64 Vx = OriginVx + Cx;
			const int64 Vy = OriginVy + Cy;
			const int32 Depth = Grid.depthAt(Vx, Vy);
			int32& Last = Impl.SweLastDepth[size_t(Cy) * size_t(Cols) + size_t(Cx)];
			if (Depth == Last)
			{
				continue;
			}
			Last = Depth;

			const int32 Bed = Grid.bedAt(Vx, Vy);
			const int64 BrickX = vxc::floorDiv(Vx, int64(vxc::WaterBrick8::kEdge));
			const int64 BrickY = vxc::floorDiv(Vy, int64(vxc::WaterBrick8::kEdge));
			const int64 ZLo = vxc::floorDiv(int64(Bed) + 1, int64(vxc::WaterBrick8::kEdge));
			const int64 ZHi = vxc::floorDiv(int64(Bed) + ScanVoxels, int64(vxc::WaterBrick8::kEdge));
			for (int64 BrickZ = ZLo; BrickZ <= ZHi; ++BrickZ)
			{
				Impl.DirtyBricks.Add(VoxelCoords::FVoxelCoord{BrickX, BrickY, BrickZ});
			}
		}
	}
}

void FlushSweIntoCA(FVoxelWaterImpl& Impl, const TCHAR* Reason)
{
	if (!Impl.SweCoupler || !Impl.SweSheet)
	{
		return;
	}
	vxc::SweGrid& Grid = *Impl.SweSheet;
	const int64_t SheetBefore = Grid.totalVolume();
	const int32 ColumnsBefore = Impl.SweCoupler->sweColumnCount();
	if (ColumnsBefore == 0 && SheetBefore == 0)
	{
		return; // armed but holding nothing -- no work, and no log noise either
	}

	int32 Demoted = 0;
	for (int32 Cy = 0; Cy < Grid.sizeY(); ++Cy)
	{
		for (int32 Cx = 0; Cx < Grid.sizeX(); ++Cx)
		{
			const int64 Vx = Grid.originVx() + Cx;
			const int64 Vy = Grid.originVy() + Cy;
			if (!Impl.SweCoupler->isSweColumn(Vx, Vy))
			{
				continue;
			}
			const int32_t Depth = Grid.depthAt(Vx, Vy);
			const int32_t Bed = Grid.bedAt(Vx, Vy);
			Impl.SweCoupler->forceDemote(Vx, Vy);
			++Demoted;
			if (Depth > 0 && Grid.depthAt(Vx, Vy) == 0)
			{
				// The units landed in CA cells stacking up from the bed, so the
				// same span MarkColumnDirty computes for an addWater() covers
				// them -- re-mesh and (on a listen server, which cannot arm this
				// anyway) replication both need them marked.
				MarkColumnDirty(Impl, Vx, Vy, Bed, uint32_t(Depth));
			}
		}
	}

	const int64_t SheetAfter = Grid.totalVolume();
	if (SheetAfter != 0)
	{
		UE_LOG(LogVoxelWater, Error,
		       TEXT("voxel.Water.SWE flush (%s): %lld of %lld sheet fill unit(s) could NOT be demoted into the CA ")
		       TEXT("(%d column(s) attempted, %d still SWE-owned). Demotion is all-or-nothing per column (swe.h S5): a ")
		       TEXT("column whose CA cells have no room this tick keeps its depth rather than half-moving. The water is ")
		       TEXT("NOT lost -- it is still in the grid and the coupled ledger still balances -- but it is NOT in the ")
		       TEXT("save blob either, so a reload will be short by that amount."),
		       Reason, (long long)SheetAfter, (long long)SheetBefore, Demoted, Impl.SweCoupler->sweColumnCount());
	}
	else
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("voxel.Water.SWE flush (%s): demoted %d column(s), %lld fill unit(s) returned to the CA through the ")
		       TEXT("ledgered demotion channel. Sheet is empty; the ADR-0005 blob format is untouched."),
		       Reason, Demoted, (long long)SheetBefore);
	}

	// The coupler's own hysteresis re-forms the sheet from here: every column
	// that still passes the eligibility predicate promotes again after
	// promoteDwellTicks (8 ticks = 0.8 s at this subsystem's fixed step). A
	// save is therefore a brief, self-healing interruption of the sheet rather
	// than a teardown, and no re-seeding code is needed on this side.
	Impl.SweLastCoupledTotal = int64_t(Impl.CA.totalVolume()) + Grid.totalVolume();
}

void MaybeRelatchImplicitOcean(FVoxelWaterImpl& Impl)
{
	const bool bWant = CVarVoxelWaterImplicitOcean.GetValueOnGameThread();
	if (bWant == Impl.bImplicitOcean)
	{
		return;
	}
	// The one safe state: nothing mobilized and no CA water anywhere, so there
	// is no cell whose ownership the change could reassign. In practice that
	// means "flip it before you touch the water", which is what a control
	// capture does anyway.
	const bool bSafe = Impl.Mob.mobilizedBricks().empty() && Impl.CA.totalVolume() == 0;
	if (!bSafe)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("voxel.Water.ImplicitOcean %d REFUSED: %llu fill unit(s) of CA water and %d "
		            "mobilized brick(s) are live, and the implicit field is what makes unmobilized "
		            "water a wall (voxelcore/waterca.h). Changing it now would either double-credit "
		            "cells the CA already owns (-> ledger shortfall) or leave mobilized sea behind "
		            "in what is supposed to be the control. Set it before the first edit, or "
		            "restart. Still running with ImplicitOcean=%d."),
		       bWant ? 1 : 0, (unsigned long long)Impl.CA.totalVolume(),
		       int(Impl.Mob.mobilizedBricks().size()), Impl.bImplicitOcean ? 1 : 0);
		// The cvar is put BACK to the value actually in force, so it never lies
		// about what the simulation is doing and so this warning fires once per
		// attempt rather than every tick.
		CVarVoxelWaterImplicitOcean->Set(Impl.bImplicitOcean, ECVF_SetByCode);
		return;
	}
	Impl.bImplicitOcean = bWant;
	// The implicit field just changed shape, so every brick the mobilizer
	// previously scanned and memoised as "no implicit water here" may now hold
	// some. That memo is a pure negative cache (waterca.h: "it changes no
	// answer, only the cost of re-asking") but a STALE one would hide the whole
	// new term, so the refusal above is also what guarantees this transition
	// only ever happens with an empty mobilizer, whose memo is empty too.
	UE_LOG(LogVoxelWater, Log,
	       TEXT("voxel.Water.ImplicitOcean = %d (watershed §6.4). The sea is %s the water "
	            "ImplicitFn; the retired Reservoir v0 does not come back either way."),
	       bWant ? 1 : 0, bWant ? TEXT("a term of") : TEXT("ABSENT from"));
}

// Constructs (or tears down) the sheet to match voxel.Water.SWE. Called once
// per Tick, before the fixed-step loop, so an arm/disarm always lands on a
// clean step boundary rather than between the coupler and the CA.
void MaybeArmSwe(FVoxelWaterImpl& Impl, UWorld* World)
{
	const bool bWant = VoxelDebug::GetWaterSwe();
	const bool bArmed = Impl.SweCoupler != nullptr;

	if (!bWant)
	{
		Impl.bSweRefusalLogged = false; // re-arm may legitimately re-refuse and should say so again
		if (bArmed)
		{
			// DISARM IS A FLUSH, NOT A DELETE. Dropping the grid would silently
			// destroy every unit of sheet depth -- the same loss the save path
			// guards against -- so the off-switch runs the identical ledgered
			// return path and only then releases the objects. That is what makes
			// this cvar genuinely reversible mid-session in both directions.
			FlushSweIntoCA(Impl, TEXT("voxel.Water.SWE 0"));
			Impl.SweCoupler.reset();
			Impl.SweSheet.reset();
			// Queued column indices only mean anything against the grid origin
			// they were computed from, and that grid is gone.
			Impl.SwePendingBedReseat.Empty();
			UE_LOG(LogVoxelWater, Log, TEXT("voxel.Water.SWE 0: shallow-water sheet disarmed; water is pure WaterCA again."));
		}
		return;
	}

	if (bArmed)
	{
		return;
	}

	// --- The ADR-0004 item 3 gate -----------------------------------------
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode != NM_Standalone)
	{
		if (!Impl.bSweRefusalLogged)
		{
			Impl.bSweRefusalLogged = true;
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("voxel.Water.SWE REFUSED: net mode is %d, not NM_Standalone. ADR-0004 (accepted 2026-07-21) item 3 ")
			       TEXT("defers enabling the CA<->SWE coupler until M3 networked water, because the coupler is a second ")
			       TEXT("simulation whose membership, dwell counters and sheet depth are 'not yet wired into the ")
			       TEXT("replication path at all' -- 'enabling it before that is a guaranteed desync'. A standalone world ")
			       TEXT("has no mirror to desync, which is the ONLY case this gate lets through. Land the replication ")
			       TEXT("plumbing first, then widen this gate -- plumbing first, flag second, as separate commits."),
			       int32(NetMode));
		}
		return;
	}

	// --- Where to put it ---------------------------------------------------
	//
	// One dense region has to be centred on something, and the honest choice is
	// wherever the water actually is: the centroid of the CA's stored bricks,
	// which for a test session is the pour. With no water stored yet, fall back
	// to the player's own viewpoint so that arming the cvar and then pouring
	// works in either order. With neither, do not guess -- retry silently next
	// frame, because a sheet centred on the world origin is 84 km from anything
	// and would only produce a confusing "armed but nothing ever promotes".
	int64 AnchorVx = 0, AnchorVy = 0;
	bool bHaveAnchor = false;
	if (Impl.CA.storedBrickCount() > 0)
	{
		double SumX = 0.0, SumY = 0.0;
		int64 Count = 0;
		for (const auto& Entry : Impl.CA.bricks())
		{
			SumX += (double(Entry.first.x) + 0.5) * double(vxc::WaterBrick8::kEdge);
			SumY += (double(Entry.first.y) + 0.5) * double(vxc::WaterBrick8::kEdge);
			++Count;
		}
		AnchorVx = int64(FMath::FloorToDouble(SumX / double(Count)));
		AnchorVy = int64(FMath::FloorToDouble(SumY / double(Count)));
		bHaveAnchor = true;
	}
	else if (APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
	{
		FVector ViewUU = FVector::ZeroVector;
		FRotator UnusedRot = FRotator::ZeroRotator;
		PC->GetPlayerViewPoint(ViewUU, UnusedRot);
		const VoxelCoords::FVoxelCoord Vc = VoxelCoords::WorldToVoxel(ViewUU);
		AnchorVx = Vc.X;
		AnchorVy = Vc.Y;
		bHaveAnchor = true;
	}
	if (!bHaveAnchor)
	{
		if (!Impl.bSweRefusalLogged)
		{
			Impl.bSweRefusalLogged = true;
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("voxel.Water.SWE: nothing to centre the sheet on yet (no stored water, no player controller). ")
			       TEXT("Retrying every frame -- pour water or wait for a pawn."));
		}
		return;
	}

	const double ArmStartSeconds = FPlatformTime::Seconds();

	// ticksPerSecond exists ONLY to scale velocityAt()'s mm/s output (swe.h S6);
	// it enters no tick rule and moves no digest. It must match this subsystem's
	// fixed step or the force field W5 eventually consumes would be reported at
	// twice its real speed, since swe.h's default assumes a 20Hz caller.
	vxc::SweConfig SheetCfg;
	SheetCfg.ticksPerSecond = int32_t(FMath::RoundToInt(1.f / FVoxelWaterImpl::FixedStepSeconds));

	const int64 OriginVx = AnchorVx - kSweSheetColumns / 2;
	const int64 OriginVy = AnchorVy - kSweSheetColumns / 2;
	Impl.SweSheet = std::make_unique<vxc::SweGrid>(OriginVx, OriginVy, kSweSheetColumns, kSweSheetColumns, SheetCfg);
	// A fresh grid means a fresh origin, so any column index left over from a
	// previous arming would now name a different column (see the member's
	// comment). The beds below are seated against the live world anyway.
	Impl.SwePendingBedReseat.Empty();

	// The wrapped solidity callback, built ONCE and shared by the bed seating
	// below and by the coupler itself, so the two can never disagree about what
	// "solid" means (see SeatSweBedZ's comment for why that matters).
	vxc::WaterCA::SolidFn Solid = Impl.Mob.makeSolidFn();

	int32 SeatedColumns = 0;
	for (int32 Cy = 0; Cy < kSweSheetColumns; ++Cy)
	{
		for (int32 Cx = 0; Cx < kSweSheetColumns; ++Cx)
		{
			const int64 Vx = OriginVx + Cx;
			const int64 Vy = OriginVy + Cy;
			const double SurfaceZUU = Impl.Terrain.GetSurfaceHeightUU(double(Vx) * VoxelCoords::VoxelSizeUU,
			                                                          double(Vy) * VoxelCoords::VoxelSizeUU);
			const int64 SurfaceVz = int64(FMath::FloorToDouble(SurfaceZUU / VoxelCoords::VoxelSizeUU));
			bool bFound = false;
			const int32 BedZ = SeatSweBedZ(Solid, Vx, Vy, SurfaceVz, bFound);
			Impl.SweSheet->setBed(Vx, Vy, BedZ);
			if (bFound)
			{
				++SeatedColumns;
			}
		}
	}

	vxc::SweCoupleConfig CoupleCfg;
	CoupleCfg.enabled = true; // the master flag swe.h ships false; this is the only place it is ever set
	Impl.SweCoupler = std::make_unique<vxc::SweCaCoupler>(*Impl.SweSheet, Impl.CA, Solid, CoupleCfg);

	// --- Seed the body that is already there -------------------------------
	//
	// forcePromote rather than waiting out promoteDwellTicks, exactly as swe.h
	// documents it for "a caller seeding a known-open body (e.g. a generated
	// lake) at world load": it runs the same promote() path, which absorbs the
	// column's CA fill at full rate through the ledgered channel, so it is
	// ledger-exact -- it is only the DWELL that is skipped, not the transfer.
	//
	// SEEDED SET = columns that have a real bed AND already hold CA water above
	// it. Note what is deliberately NOT done: the coupler's `eligible()`
	// predicate is private, and re-implementing it engine-side to pre-filter
	// this set would fork a rule that lives in golden-pinned code, which is
	// exactly how two copies of a predicate drift. Instead this seeds the water
	// -bearing columns and lets the machinery correct itself: a seeded column
	// that in fact fails the predicate (a lidded pocket, a one-voxel pipe)
	// accumulates the ineligible dwell and demotes all-or-nothing after
	// demoteDwellTicks (32 ticks = 3.2 s), returning its units through the same
	// ledger. The cost of being wrong is a few seconds of a column being
	// simulated by the wrong solver; the cost of a forked predicate is silent
	// disagreement forever.
	int32 SeededColumns = 0;
	int64_t SeededVolume = 0;
	const int32 ScanVoxels = Impl.SweCoupler->config().sheetScanVoxels;
	for (int32 Cy = 0; Cy < kSweSheetColumns; ++Cy)
	{
		for (int32 Cx = 0; Cx < kSweSheetColumns; ++Cx)
		{
			const int64 Vx = OriginVx + Cx;
			const int64 Vy = OriginVy + Cy;
			const int32_t BedZ = Impl.SweSheet->bedAt(Vx, Vy);
			if (Solid(Vx, Vy, BedZ) == vxc::MAT_AIR)
			{
				continue; // no bed was found in the search window: not a sheet column
			}
			bool bHasWater = false;
			for (int32 K = 1; K <= ScanVoxels && !bHasWater; ++K)
			{
				bHasWater = Impl.CA.fillAt(Vx, Vy, int64(BedZ) + K) != 0;
			}
			if (!bHasWater)
			{
				continue;
			}
			Impl.SweCoupler->forcePromote(Vx, Vy);
			if (Impl.SweCoupler->isSweColumn(Vx, Vy))
			{
				++SeededColumns;
				SeededVolume += Impl.SweSheet->depthAt(Vx, Vy);
				// Those cells just emptied on the CA side; the mesh has to lose
				// them in the same tick or the water renders twice.
				MarkColumnDirty(Impl, Vx, Vy, int64(BedZ), uint32_t(Impl.SweSheet->depthAt(Vx, Vy)));
			}
		}
	}

	// Seat the conservation ledger. Promotion is a TRANSFER, so the sum across
	// both solvers is unchanged by the seeding above -- which is itself the
	// first thing this ledger asserts, on the very next fixed step.
	Impl.SweLastCoupledTotal = int64_t(Impl.CA.totalVolume()) + Impl.SweSheet->totalVolume();
	Impl.SweInjected = Impl.SweLastCoupledTotal;
	Impl.SweConservationFailures = 0;
	Impl.bSweRefusalLogged = false;

	const double ArmMs = (FPlatformTime::Seconds() - ArmStartSeconds) * 1000.0;
	UE_LOG(LogVoxelWater, Log,
	       TEXT("voxel.Water.SWE ARMED (ADR-0004, NM_Standalone only): %dx%d sheet at voxel origin (%lld,%lld), ")
	       TEXT("%d/%d column(s) found a bed, %d column(s) seeded by forcePromote holding %lld fill unit(s). ")
	       TEXT("Arming took %.1f ms (bed seating). kSweVersion=%u, kWaterCAVersion=%u -- neither is bumped and no ")
	       TEXT("water golden moves. The renderer draws the UNION of sheet depth and CA fill (ADR-0004 'Renderer', ")
	       TEXT("see UnionSweFill), so promoted water is visible. NOTE it will not look identical to the CA-only ")
	       TEXT("case: the sheet spreads a pour laterally as a thin film where Phase C pools it into basins. That ")
	       TEXT("is a real difference in the physics, not a rendering artifact -- watch the conservation line."),
	       kSweSheetColumns, kSweSheetColumns, (long long)OriginVx, (long long)OriginVy, SeatedColumns,
	       kSweSheetColumns * kSweSheetColumns, SeededColumns, (long long)SeededVolume, ArmMs, vxc::kSweVersion,
	       vxc::kWaterCAVersion);
}

// One fixed 10Hz step: Reservoir v0 top-up, then vxc::WaterCA::step(),
// folding the CA's own post-step active set into the dirty-for-remesh /
// dirty-for-replication sets (task items 2 & 3).
// --- W3 rivers (voxel.Water.Rivers, plan S3.7 Layer R) ----------------------
//
// HOW BIG A REGION, AND WHY IT IS BUILT ONCE. rivernet.h's generation is a D8
// flow accumulation over an inclusive TILE-PIXEL rectangle, and a pixel is 30 m
// -- so 48x48 pixels is a 1.44 km square, big enough to hold a whole small
// catchment and its outlet and cheap enough to build in one frame (2,304 pixel
// samples plus one sort). It is built ONCE, around the arming anchor, and is
// not re-centred as the player walks: a rebuild would reset every segment's
// storage and discard every promotion, which is a far worse artefact than a
// river that stops at the edge of the region it was built for. Re-centring
// needs an incremental generation pass and the diff log persisted first; both
// are follow-ups, and this v0 is the same shape as the SWE sheet's own single
// dense region.
//
// The region edge is not a special case in the graph: rivernet.h's D8 never
// samples an out-of-bounds neighbour, so a pixel whose true downhill path would
// leave the region simply becomes a terminal outlet -- the graph drains to the
// region's low edge with no code for it.
constexpr int64 kRiverRegionPixels = 48;

// Divides the raw catchment accumulation to get a per-tick baseflow. See
// FVoxelWaterImpl::RiverBaseflow for why the raw number cannot be used.
constexpr int64 kRiverBaseflowDivisor = 256;
constexpr int64 kRiverBaseflowMax = 65536;

// Constructs (or tears down) the river graph + coupler to match
// voxel.Water.Rivers. Called once per Tick, before the fixed-step loop, for the
// same reason MaybeArmSwe is: an arm/disarm lands on a clean step boundary.
void MaybeArmRivers(FVoxelWaterImpl& Impl, UWorld* World)
{
	const bool bWant = VoxelDebug::GetWaterRivers();
	const bool bArmed = Impl.RiverCoupler != nullptr;

	if (!bWant)
	{
		Impl.bRiverRefusalLogged = false;
		if (bArmed)
		{
			// DISARM IS A PLAIN DELETE HERE, unlike voxel.Water.SWE's disarm,
			// which has to flush the sheet back into the CA first. The
			// difference is the coupling's direction: every unit this coupler
			// ever took out of the graph is ALREADY in the CA (or ledgered as
			// gone to sea), and what remains in the graph is segment storage --
			// a routing state variable at tile scale, not water anybody can see
			// or swim in. Dropping it destroys no visible water. What it does
			// destroy is the routing history and any promotions, which is
			// exactly what "the graph is not persisted yet" already means.
			UE_LOG(LogVoxelWater, Log,
			       TEXT("voxel.Water.Rivers 0: river graph disarmed (%u segments, %lld promotions, %lld graph units ")
			       TEXT("delivered to the CA, %lld to the ocean). Water already handed to the CA stays; the graph's ")
			       TEXT("own routing state does not persist."),
			       Impl.Rivers ? Impl.Rivers->segmentCount() : 0u, (long long)Impl.RiverPromotions,
			       (long long)Impl.RiverCoupler->graphUnitsToCA(),
			       (long long)Impl.RiverCoupler->graphUnitsToOcean());
			Impl.RiverCoupler.reset();
			Impl.Rivers.reset();
			Impl.RiverBaseflow.clear();
		}
		return;
	}

	if (bArmed)
	{
		return;
	}

	// --- the net-mode gate ------------------------------------------------
	//
	// NM_Client only, deliberately NOT narrowed to NM_Standalone the way
	// MaybeArmSwe is. ADR-0004 item 3 refuses the SWE coupler on a networked
	// world because that coupler holds simulation state a client must SEE
	// (sheet depth is drawn water) and none of it replicates. This coupler
	// holds no such state: its only client-visible output is WaterCA fill,
	// which already replicates through BroadcastWaterDiffs, and the graph is
	// server-side bookkeeping a client never reads. So a listen or dedicated
	// server may run it and its clients mirror the resulting water exactly as
	// they mirror a pour. A client running its own would be simulating on top
	// of a mirror, which is the thing every other write path here refuses.
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		if (!Impl.bRiverRefusalLogged)
		{
			Impl.bRiverRefusalLogged = true;
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("voxel.Water.Rivers REFUSED on NM_Client: rivers tick server-side (plan S3.7 Layer R, ")
			       TEXT("'segment graph ticks ~1Hz server-side'). A client mirrors the resulting water through the ")
			       TEXT("existing water-diff channel and must not simulate its own."));
		}
		return;
	}

	// --- where to build it -------------------------------------------------
	// The player's viewpoint, not the CA's centroid: a river network has to
	// exist BEFORE there is any water to take a centroid of, which is the
	// opposite of the SWE sheet's situation.
	int64 AnchorVx = 0, AnchorVy = 0;
	bool bHaveAnchor = false;
	if (APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
	{
		FVector ViewUU = FVector::ZeroVector;
		FRotator UnusedRot = FRotator::ZeroRotator;
		PC->GetPlayerViewPoint(ViewUU, UnusedRot);
		const VoxelCoords::FVoxelCoord Vc = VoxelCoords::WorldToVoxel(ViewUU);
		AnchorVx = Vc.X;
		AnchorVy = Vc.Y;
		bHaveAnchor = true;
	}
	if (!bHaveAnchor)
	{
		if (!Impl.bRiverRefusalLogged)
		{
			Impl.bRiverRefusalLogged = true;
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("voxel.Water.Rivers: no player controller to anchor the graph on yet. Retrying every frame."));
		}
		return;
	}

	const double ArmStartSeconds = FPlatformTime::Seconds();

	const int64 PixelSizeMm = int64(Impl.Tiles.pixelSizeMm());
	const int64 AnchorPx = vxc::floorDiv(AnchorVx * int64(vxc::kVoxelSizeMm), PixelSizeMm);
	const int64 AnchorPy = vxc::floorDiv(AnchorVy * int64(vxc::kVoxelSizeMm), PixelSizeMm);
	vxc::RegionBounds Bounds;
	Bounds.px0 = AnchorPx - kRiverRegionPixels / 2;
	Bounds.py0 = AnchorPy - kRiverRegionPixels / 2;
	Bounds.px1 = Bounds.px0 + kRiverRegionPixels - 1;
	Bounds.py1 = Bounds.py0 + kRiverRegionPixels - 1;

	Impl.Rivers = std::make_unique<vxc::RiverNetwork>();
	Impl.Rivers->buildFromFlowAccumulation(Impl.Tiles, Impl.Terrain.GetSeed(), Bounds);

	const uint32 SegCount = Impl.Rivers->segmentCount();
	if (SegCount == 0)
	{
		// A flat or uniformly-dry 1.44 km square genuinely has no river in it.
		// Say so once and stop retrying every frame against the same terrain,
		// but leave the graph null so walking somewhere else and re-arming works.
		Impl.Rivers.reset();
		if (!Impl.bRiverRefusalLogged)
		{
			Impl.bRiverRefusalLogged = true;
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("voxel.Water.Rivers: no segments crossed the catchment threshold in the %lldx%lld-pixel ")
			       TEXT("region around voxel (%lld,%lld) -- this terrain has no river here. Toggle the cvar off and ")
			       TEXT("on somewhere wetter."),
			       (long long)kRiverRegionPixels, (long long)kRiverRegionPixels, (long long)AnchorVx,
			       (long long)AnchorVy);
		}
		return;
	}

	Impl.RiverBaseflow.assign(SegCount, 0);
	for (uint32 S = 0; S < SegCount; ++S)
	{
		const int64 Raw = int64(Impl.Rivers->segments()[S].discharge); // build-time catchment estimate
		Impl.RiverBaseflow[S] = int32_t(vxc::clampi64(Raw / kRiverBaseflowDivisor, 1, kRiverBaseflowMax));
	}

	vxc::RiverCoupleConfig Cfg;
	Cfg.enabled = true;
	// Everything else stays at rivercouple.h's documented defaults on purpose:
	// they are derived there against real units (a 30 m pixel, a 10 cm voxel, a
	// 1 Hz tick), and re-deciding them here would put two sources of truth on
	// numbers whose derivations are written down in exactly one place.
	Impl.RiverCoupler = std::make_unique<vxc::RiverCaCoupler>(*Impl.Rivers, Impl.CA, Impl.Tiles,
	                                                          Impl.Mob.makeSolidFn(), Cfg);
	Impl.RiverLastTickWorldSeconds = -1000.0;
	Impl.RiverLastStatusWorldSeconds = -1000.0;
	Impl.RiverPromotions = 0;
	Impl.bRiverRefusalLogged = false;

	// --- Phase 2: the graph is now the persisted scalar authority ------------
	//
	// Recording FIRST, so a dam placed one tick after the arm is in the log.
	Impl.Rivers->setDiffRecording(true);

	// Then the held blob, if this session loaded one. CONSUMED EXACTLY ONCE
	// (see PendingHydroGraphBlob): a second arm over different bounds builds a
	// different graph, restoreRoutingState would refuse it, and re-offering the
	// bytes every toggle would turn one honest refusal into a log spam.
	if (!Impl.PendingHydroGraphBlob.empty())
	{
		std::vector<uint8_t> Blob;
		Blob.swap(Impl.PendingHydroGraphBlob);
		if (vxc::RiverNetState::load(Blob.data(), Blob.size(), *Impl.Rivers))
		{
			UE_LOG(LogVoxelWater, Log,
			       TEXT("voxel.Water.Rivers: restored the saved graph state -- %llu diff(s) replayed, %lld storage ")
			       TEXT("units back in the reaches (injected %lld, out to outlets %lld)."),
			       (unsigned long long)Impl.Rivers->diffLog().size(), (long long)Impl.Rivers->totalStorage(),
			       (long long)Impl.Rivers->totalInjected(), (long long)Impl.Rivers->totalOutflowToOutlets());
		}
		else
		{
			// LOUD, and specifically about WHY: the overwhelmingly likely cause
			// is that the graph was saved anchored somewhere else, because the
			// region is centred on the player's viewpoint at arm time.
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("voxel.Water.Rivers: the saved graph state was REFUSED (%d bytes, this graph has %u ")
			       TEXT("segments). Most likely it was recorded over DIFFERENT REGION BOUNDS -- the region is ")
			       TEXT("centred on the player's viewpoint at arm time -- or by a different kRiverNetVersion ")
			       TEXT("(engine is now v%u). Dams and in-flight water from that session are gone; the graph ")
			       TEXT("itself is fine and starts empty."),
			       int32(Blob.size()), Impl.Rivers->segmentCount(), vxc::kRiverNetVersion);
			// A partial diff replay may have landed (RiverNetState::load says
			// so). Rebuild from scratch so the graph is not a half-replayed
			// hybrid of two sessions.
			Impl.Rivers->buildFromFlowAccumulation(Impl.Tiles, Impl.Terrain.GetSeed(), Bounds);
			Impl.Rivers->setDiffRecording(true);
		}
	}

	UE_LOG(LogVoxelWater, Log,
	       TEXT("voxel.Water.Rivers 1: built a %lldx%lld-pixel (%lld m) river graph around voxel (%lld,%lld): ")
	       TEXT("%d nodes, %u segments, in %.2f ms. Ticking at 1 Hz; the ocean (voxel z=0) is the sink."),
	       (long long)kRiverRegionPixels, (long long)kRiverRegionPixels,
	       (long long)(kRiverRegionPixels * PixelSizeMm / 1000), (long long)AnchorVx, (long long)AnchorVy,
	       int32(Impl.Rivers->nodes().size()), SegCount,
	       (FPlatformTime::Seconds() - ArmStartSeconds) * 1000.0);
}

void StepFixed(FVoxelWaterImpl& Impl, double NowWorldSeconds)
{
	// ADR-0003 item 4: re-check the cvar every fixed step (cheap -- a bool
	// compare, plus a memo clear only on the OFF-transition, see
	// WaterCA::setSolidCacheEnabled) so voxel.Water.SolidCacheEnabled can be
	// flipped live without restarting. Must happen before step() below --
	// hydrostaticPass reads solidCacheEnabled_ at the top of its own call.
	Impl.CA.setSolidCacheEnabled(CVarVoxelWaterSolidCache.GetValueOnGameThread());

	// THE MOBILIZED CEILING (§6.5.5, work item 9b), re-read every fixed step for
	// the same reason the solid cache is: it is pure policy, never persisted,
	// never digested and never replicated (waterca.h), so it can be retuned live
	// without a restart and without touching determinism. What replicates is the
	// mobilized set the ceiling shapes, not the ceiling.
	//
	// Set BEFORE advanceFront below, which is the only thing it bounds. Edits
	// and markMobilized are exempt by design and may legitimately push
	// `mobilized_` past it -- which is why atMobilizedCeiling() is a `>=` test.
	{
		const int32 Ceiling = CVarVoxelWaterMobilizedCeiling.GetValueOnGameThread();
		Impl.Mob.setMobilizedCeiling(Ceiling > 0 ? size_t(Ceiling) : size_t(0));
	}

	// RESERVOIR v0 USED TO BE HERE, and its retirement is watershed plan work
	// item 8 (§6.4: "the bespoke reservoir top-up at :3307-3315 can retire once
	// proven equivalent"). It was NOT equivalent. It was a set of breach voxels
	// pinned to 255 fill units every tick, forever, and voxel-core's
	// tests/test_ocean.cpp measures the three things that made it worth
	// deleting rather than fixing:
	//
	//   * THE SEA WAS NOT WATER TO THE SIMULATION. Only the pinned cells were.
	//     The ocean beyond them was open air, so a cove cut into the coast did
	//     not fill, it DRAINED -- and because the source never turned off, the
	//     total volume and the CA's ACTIVE BRICK COUNT both climbed for as long
	//     as the world ran. Measured on a 4x4 breach: 358k fill units and 536
	//     active bricks at tick 100, 610k and 758 at tick 1500, still rising.
	//     That was a live perf leak behind every below-sea-level dig.
	//   * IT COULD NOT TELL A DUG PIT FROM THE SEA. Its rule was "cleared, below
	//     the datum, touching non-solid space that is also below the datum",
	//     which the second pass of an ordinary inland dig satisfies against the
	//     first pass's own air. Measured: an infinite spring in dry rock 50
	//     voxels inland.
	//   * ITS HEAD WAS THE BREACH, NOT THE DATUM. A cell pinned at 255 at
	//     z = -9 voxels is a pressure source at z = -9.
	//
	// Nothing replaces it here, because nothing needs to: the sea is now a term
	// of the implicit field (see FVoxelWaterImpl's ImplicitFn), a breach
	// funnels through NotifyTerrainRegionEdited -> mobilizeEditRegion exactly
	// as a dug lake shore does, and the front below carries it from there.

	// C8 mobilize-on-approach: advance the implicit->CA front BEFORE stepping,
	// so any brick this tick's water could flow into is already CA-owned. Over
	// budget simply defers — a deferred brick is still a wall to the CA, so
	// deferring can never leak water (waterca.h). Cheap when nothing is active:
	// a settled or empty CA has no active bricks, so no front to advance.
	if (Impl.Mob.advanceFront(Impl.CA) > 0)
	{
		MarkMobilizedBricksDirty(Impl);
	}

	// --- W3: the river network's ~1 Hz tick (plan S3.7 Layer R) ------------
	//
	// PLACED HERE -- with the reservoir top-up and the mobilizer front, and
	// BEFORE the coupled SWE window below -- for exactly the reason that
	// window's own comment gives: every injection into the CA has to happen
	// OUTSIDE it, or the ADR-0004 ledger cannot tell an injection from a leak.
	// The river coupler is an injector (discharge becomes fill), so it belongs
	// on this side of the line, and the `Before - SweLastCoupledTotal` fold-in
	// below then accounts for it automatically with no river-specific
	// bookkeeping at all.
	//
	// ~1 Hz off world time rather than every fixed step, because that is the
	// cadence plan S3.7 specifies for Layer R, it is what rivernet.h's
	// Muskingum travel-time constants were derived against, and it means the
	// promotion detector's sampling cost (rivercouple.h section 4: a hard
	// budget of order 10^5 hashed CA probes per pass, independent of how big
	// the graph is) is paid once a second instead of ten times.
	// SILL FAUCETS (Phase 2). Whatever the basin ledger overflowed since the
	// last step joins the reach nearest its baked outlet -- the first consumer,
	// ever, of `BasinEntry::outletX/Y`, which has been on the wire since
	// bake_ver 8 and read by nothing. Ledgered on both sides: what the graph
	// will not take is refunded to the lake, which then sits above its sill and
	// back-pressures, rather than evaporating.
	//
	// OUTSIDE the river block, and every fixed step rather than at the graph's
	// 1 Hz, precisely BECAUSE the common case is that there is no graph
	// (voxel.Water.Rivers is off by default). A queue that were only drained
	// when a graph existed would grow for the whole session in every default
	// run. Draining it here refunds instead, which is both bounded and the
	// honest answer: a lake with nowhere to spill to rises past its sill.
	// Costs one empty-vector test per step when nothing has overflowed.
	RouteBasinSpills(Impl);

	if (Impl.RiverCoupler && Impl.Rivers &&
	    NowWorldSeconds - Impl.RiverLastTickWorldSeconds >= FVoxelWaterImpl::RiverStepSeconds)
	{
		Impl.RiverLastTickWorldSeconds = NowWorldSeconds;

		// Baseflow: rain on every reach's own catchment, every tick. Only the
		// segments that existed at BUILD time are fed this way -- a promoted
		// channel deliberately gets nothing, because it is fed by the
		// bifurcation split at its take-off node (rivernet.h "Bifurcation"),
		// and giving it a catchment of its own as well would invent water.
		const uint32 FedSegments = FMath::Min(Impl.Rivers->segmentCount(), uint32(Impl.RiverBaseflow.size()));
		for (uint32 S = 0; S < FedSegments; ++S)
		{
			Impl.Rivers->injectInflow(S, Impl.RiverBaseflow[S]);
		}

		Impl.Rivers->step(int64_t(FVoxelWaterImpl::RiverStepSeconds * 1000.0));
		Impl.RiverCoupler->step();

		// Re-mesh what the coupler actually wrote. The CA's own changed-brick
		// diff further down would catch these eventually, but only after the
		// NEXT ca.step(), which is a frame of visibly absent water at every
		// outfall once a second -- and the reservoir top-up directly above
		// already establishes the rule that an injector dirties its own columns.
		for (const vxc::RiverOutfallWrite& W : Impl.RiverCoupler->lastOutfallWrites())
		{
			MarkColumnDirty(Impl, W.vx, W.vy, W.vz, W.placed);
		}

		// Promotions are TOPOLOGY, not water. The diffs are the persistence /
		// replication feed rivernet.h's graph-diff log describes; nothing
		// consumes them on disk yet, so drain and count them rather than let the
		// queue grow unbounded across a session -- and log each one, because a
		// promotion permanently re-routes a river and should never be silent.
		const std::vector<vxc::RiverDiffRecord> Diffs = Impl.RiverCoupler->takePendingDiffs();
		Impl.RiverPromotions += int64(Diffs.size());
		for (const vxc::RiverDiffRecord& D : Diffs)
		{
			UE_LOG(LogVoxelWater, Log,
			       TEXT("voxel.Water.Rivers: PROMOTED a channel from node %u over %d pixels (kDivertChannel). ")
			       TEXT("Sustained CA flux down a course that was not a segment is now one; the take-off node ")
			       TEXT("bifurcates and its inflow splits evenly. NOT PERSISTED -- this is lost on reload."),
			       D.headNode, int32(D.course.size()));
		}

		// 0.2 Hz status. Deliberately slower than the river tick itself, so this
		// is a heartbeat rather than a transcript.
		if (NowWorldSeconds - Impl.RiverLastStatusWorldSeconds >= 5.0)
		{
			Impl.RiverLastStatusWorldSeconds = NowWorldSeconds;
			UE_LOG(LogVoxelWater, Log,
			       TEXT("RiverPerf: segments=%u storage=%lld outlets=%lld toCA=%lld toOcean=%lld refunded=%lld ")
			       TEXT("caFill=%lld candidates=%d bestDwell=%d promotions=%lld injected=%lld"),
			       Impl.Rivers->segmentCount(), (long long)Impl.Rivers->totalStorage(),
			       (long long)Impl.Rivers->totalOutflowToOutlets(),
			       (long long)Impl.RiverCoupler->graphUnitsToCA(),
			       (long long)Impl.RiverCoupler->graphUnitsToOcean(),
			       (long long)Impl.RiverCoupler->graphUnitsRefunded(),
			       (long long)Impl.RiverCoupler->fillDeliveredToCA(),
			       Impl.RiverCoupler->trackedCandidateCount(), Impl.RiverCoupler->bestCandidateDwell(),
			       (long long)Impl.RiverPromotions, (long long)Impl.Rivers->totalInjected());

			// The graph's own four-way ledger, checked live for the same reason
			// the SWE one is: rivercouple.h's tests cover the coupler, and
			// nothing covers the wiring in THIS file that decides what to inject.
			const int64_t Closed = Impl.Rivers->totalStorage() + Impl.Rivers->totalOutflowToOutlets() +
			                       Impl.Rivers->totalWithdrawnToCoupler();
			if (Closed != Impl.Rivers->totalInjected())
			{
				UE_LOG(LogVoxelWater, Error,
				       TEXT("voxel.Water.Rivers ledger FAILED: storage+outlets+withdrawn=%lld, injected=%lld, ")
				       TEXT("delta=%+lld"),
				       (long long)Closed, (long long)Impl.Rivers->totalInjected(),
				       (long long)(Closed - Impl.Rivers->totalInjected()));
			}
		}
	}

	// --- W4: the coupled shallow-water half of the step (ADR-0004) ---------
	//
	// TICK ORDER IS coupler.step(); grid.step(); ca.step(); AND IT IS COPIED,
	// NOT REASONED OUT. Every coupling test in voxel-core/tests/test_swe.cpp --
	// swe_coupler_conserves_volume_across_the_boundary,
	// swe_coupler_puncture_meters_the_inrush_then_hands_the_column_over,
	// swe_coupler_ownership_partition_walls_off_and_evacuates -- drives exactly
	// this sequence, so it is the only order the golden-pinned behaviour has
	// ever been observed under. It is also the order the design wants: decide
	// ownership and move water across the boundary FIRST (so no cell is in two
	// domains for any part of the tick), then advance each solver over a
	// partition that is already settled for this tick.
	//
	// PLACED HERE, immediately before the existing CA step, rather than at the
	// top of StepFixed: the reservoir top-up and the C8 mobilize-on-approach
	// front above both INJECT into the CA, and running them before the coupler
	// means an injection is available to be absorbed on the same tick it lands
	// instead of sitting inside a sheet's z-range for a tick (the "bounded
	// standing residue" swe.h S5 describes, which a renderer would have to draw).
	// It also puts every external injection outside the coupled window, which is
	// what makes the ledger below able to tell an injection from a leak.
	const bool bSweArmed = Impl.SweCoupler != nullptr && Impl.SweSheet != nullptr;
	if (bSweArmed)
	{
		const int64_t Before = int64_t(Impl.CA.totalVolume()) + Impl.SweSheet->totalVolume();
		// Everything that moved the coupled total since the last window closed
		// came from outside the coupler by definition: spawn, breach, reservoir
		// top-up, mobilization crediting implicit cavern water, or a placement
		// destroying some. Fold it in as injection so the invariant below is
		// about THIS tick's three calls and nothing else.
		Impl.SweInjected += Before - Impl.SweLastCoupledTotal;

		// INSIDE the window, and first: a column whose ground a terrain edit
		// removed has to get its bed back before the coupler decides ownership
		// against it, or the edit costs the sheet that column permanently (see
		// ReseatEditedSweBeds). It moves no water, which is precisely why it is
		// safe to run here -- and running it here is what makes the invariant
		// below prove that, every tick, instead of taking it on trust.
		ReseatEditedSweBeds(Impl);

		Impl.SweCoupler->step();
		Impl.SweSheet->step();

		MarkSweDepthChangesDirty(Impl);
	}

	Impl.CA.step();
	++Impl.StepsThisWindow;
	Impl.LastSteppedBrickCount = int32(Impl.CA.steppedBrickCount());

	if (bSweArmed)
	{
		// ADR-0004's headline invariant, checked every tick:
		//     ca.totalVolume() + grid.totalVolume() == total injected
		//
		// WHY THIS EXISTS WHEN THE NUMERICS ARE ALREADY GOLDEN-PINNED. swe.h's
		// conservation is structural and test_swe.cpp asserts it per tick with
		// an independent re-sum. None of that covers a single line of the code
		// in THIS file, which is where the grid gets built, where the beds get
		// seated, which columns get seeded, and when the sheet gets flushed --
		// all of it new, none of it under test, and every one of those the kind
		// of thing that loses water quietly. A wrong bed or a double-counted
		// seed shows up here on the first tick rather than as a shallower lake
		// after a reload.
		//
		// ensureMsgf, not check: this is a dev cvar and a conservation slip is a
		// bug to investigate, not a reason to take an editor session down with
		// it. Fires once per unique site by construction, and the Error log
		// below carries the numbers on every occurrence.
		const int64_t After = int64_t(Impl.CA.totalVolume()) + Impl.SweSheet->totalVolume();
		Impl.SweLastCoupledTotal = After;
		if (After != Impl.SweInjected)
		{
			++Impl.SweConservationFailures;
			const int64_t Delta = After - Impl.SweInjected;
			ensureMsgf(false,
			           TEXT("voxel.Water.SWE conservation FAILED: ca+grid=%lld, injected=%lld, delta=%+lld"),
			           (long long)After, (long long)Impl.SweInjected, (long long)Delta);
			UE_LOG(LogVoxelWater, Error,
			       TEXT("voxel.Water.SWE conservation FAILED on the coupled step (ADR-0004 invariant ca.totalVolume() + ")
			       TEXT("grid.totalVolume() == injected): ca=%llu + sheet=%lld = %lld, expected %lld, delta %+lld ")
			       TEXT("(failure #%lld). The coupled window is exactly coupler.step(); grid.step(); ca.step() -- every ")
			       TEXT("other injection is folded in outside it -- so this is the engine wiring in ")
			       TEXT("VoxelWaterSubsystem.cpp, not swe.h's numerics."),
			       (unsigned long long)Impl.CA.totalVolume(), (long long)Impl.SweSheet->totalVolume(), (long long)After,
			       (long long)Impl.SweInjected, (long long)Delta, (long long)Impl.SweConservationFailures);
			// Re-seat so the next tick reports its OWN delta rather than
			// re-reporting this one forever. The running total stays honest
			// about where the water actually is.
			Impl.SweInjected = After;
		}

		// 1Hz status, on the water log rather than the perf log: this is a
		// correctness/diagnostic line for a dev flag, not a budget number, and
		// while the renderer cannot draw sheet depth it is the ONLY way to see
		// that promoted water still exists.
		if (NowWorldSeconds - Impl.SweLastStatusWorldSeconds >= 1.0)
		{
			Impl.SweLastStatusWorldSeconds = NowWorldSeconds;
			const vxc::SweVelocity V = Impl.SweSheet->velocityAt(
				Impl.SweSheet->originVx() + Impl.SweSheet->sizeX() / 2,
				Impl.SweSheet->originVy() + Impl.SweSheet->sizeY() / 2);
			UE_LOG(LogVoxelWater, Log,
			       TEXT("SwePerf: sweColumns=%d sheetVolume=%lld caVolume=%llu punctured=%d toCA=%lld toSWE=%lld ")
			       TEXT("bedsReseated=%lld pendingReseats=%d centreVel=(%d,%d) mm/s conservationFailures=%lld ")
			       TEXT("(sheet volume IS drawn -- ADR-0004 'Renderer' union is wired; see UnionSweFill)"),
			       Impl.SweCoupler->sweColumnCount(), (long long)Impl.SweSheet->totalVolume(),
			       (unsigned long long)Impl.CA.totalVolume(), Impl.SweCoupler->lastPuncturedCount(),
			       (long long)Impl.SweCoupler->transferredToCA(), (long long)Impl.SweCoupler->transferredToSWE(),
			       (long long)Impl.SweBedsReseated, Impl.SwePendingBedReseat.Num(),
			       V.xMmPerSec, V.yMmPerSec, (long long)Impl.SweConservationFailures);
		}
	}

	// W5: the active set is now a RENDER input as well as a re-mesh trigger --
	// it is the foam signal, written into vertex colour A by both paths (see
	// FVoxelWaterImpl::ActiveBricks and ApplyWaterBrickPooled's Params).
	//
	// THE SETTLE EDGE IS THE WHOLE REASON THIS IS A DIFF AND NOT A LOOP. A brick
	// that stops being active stops being marked dirty, so it stops being
	// re-meshed -- and its last re-mesh was one where it WAS active. Foam would
	// therefore switch on correctly and then never switch off, freezing whitewater
	// onto water that has been perfectly still for minutes. Marking the bricks
	// that LEFT the set dirty costs exactly one extra re-mesh per brick per
	// activity episode, and it is the cheapest possible fix: the alternative is a
	// per-brick decay timer that has to re-publish on a schedule of its own.
	//
	// Deliberately NOT added to DirtySinceLastBroadcast: a settle carries no fill
	// change, so replicating it would spend bandwidth on a diff that is empty.
	TSet<VoxelCoords::FVoxelCoord> NowActive;
	NowActive.Reserve(int32(Impl.CA.activeBricks().size()));
	for (const vxc::BrickKey& K : Impl.CA.activeBricks())
	{
		const VoxelCoords::FVoxelCoord C = ToCoord(K);
		NowActive.Add(C);
		Impl.DirtyBricks.Add(C);
		Impl.DirtySinceLastBroadcast.Add(C);
	}
	for (const VoxelCoords::FVoxelCoord& C : Impl.ActiveBricks)
	{
		if (!NowActive.Contains(C))
		{
			Impl.DirtyBricks.Add(C);
		}
	}
	Impl.ActiveBricks = MoveTemp(NowActive);

	// --- THE RETURN PATH (§6.5.3, work item 9c) -----------------------------
	//
	// Mobilization promotes implicit -> CA; until this call nothing ever went
	// the other way. `active_` settles to empty, but `mobilized_` is persisted,
	// replicated and grew forever -- a memory leak with extra steps, and the
	// failure mode §6.5.2 names Dwarf Fortress for shipping.
	//
	// PLACED AFTER step() AND AFTER THE ACTIVE-SET DIFF, deliberately. The
	// predicate's condition (b) is "k is not active and no 6-face neighbour is
	// active", so it must read the active set this tick LEFT BEHIND, not the one
	// it started with -- running it before step() would test a stale
	// neighbourhood and demote a brick the tick was about to wake.
	//
	// AUTHORITY ONLY. StepFixed is only reached from Tick's NetMode != NM_Client
	// branch, so this is already unreachable on a client; the explicit flag is
	// what makes the doctrine legible at the call site, since voxel-core is
	// netmode-free and cannot enforce it itself.
	if (Impl.bAuthority)
	{
		const int32 DemoteBudget = CVarVoxelWaterDemoteBudget.GetValueOnGameThread();
		if (DemoteBudget > 0)
		{
			Impl.Mob.demoteBudgeted(Impl.CA, size_t(DemoteBudget));
		}
	}

	// DRAIN MOBILIZED FIRST, THEN DEMOTED, and waterca.h is explicit that the
	// order is the contract: a brick that mobilized and demoted inside one frame
	// must land on "implicit", not on "mobilized". Condition (b) makes that
	// sequence very nearly unreachable (a freshly mobilized brick is filled and
	// woken, hence active), but the order is free to get right here and
	// expensive to discover in a desync months later.
	MarkMobilizedBricksDirty(Impl);
	for (const vxc::BrickKey& K : Impl.Mob.takeRecentlyDemoted())
	{
		// Re-mesh: the brick must stop being drawn as CA water and start being
		// drawn as implicit water. RefreshImplicitWater re-adds the implicit
		// component on its own sweep; this side just has to stop drawing the CA
		// one, and the brick's fill is already gone (clearBrickFill collapsed it
		// out of the map), so the ordinary emptied-brick re-mesh path handles it.
		Impl.DirtyBricks.Add(ToCoord(K));
		// Replicate as an EXPLICIT key removal. It must never be inferred
		// client-side (waterca.h): the client's evidence for mobilization is the
		// ARRIVAL of fill, and a demotion's signature is fill that stops arriving
		// -- indistinguishable from a dropped packet.
		//
		// Only queued when something will actually drain it. A standalone world
		// demotes identically; it simply has no clients to tell, and a queue
		// nobody reads is an unbounded leak in exactly the configuration that
		// runs longest.
		if (Impl.bReplicating)
		{
			Impl.PendingRemovals.push_back(K);
		}
	}

	// A BACKSTOP ON THE BACKLOG. The queue drains at ~5 Hz against a byte cap,
	// so it is bounded in every healthy case; this catches the unhealthy one (a
	// networked world with no AVoxelEditRelay, which already warns per
	// broadcast). Dropping the OLDEST is the right end to drop from: those are
	// the removals a client is most likely to have already been told about by a
	// later full-brick diff, and keeping the newest keeps the queue converging
	// on the present rather than replaying a stale past.
	constexpr size_t kMaxPendingRemovals = 1u << 16;
	if (Impl.PendingRemovals.size() > kMaxPendingRemovals)
	{
		const size_t Dropped = Impl.PendingRemovals.size() - kMaxPendingRemovals;
		Impl.PendingRemovals.erase(Impl.PendingRemovals.begin(),
		                           Impl.PendingRemovals.begin() + ptrdiff_t(Dropped));
		UE_LOG(LogVoxelWater, Error,
		       TEXT("Water key-removal backlog exceeded %llu -- dropped %llu oldest. Clients may hold bricks the ")
		       TEXT("authority has demoted until a later diff covers them. This means removals are not being ")
		       TEXT("broadcast at all; check for a missing AVoxelEditRelay."),
		       (unsigned long long)kMaxPendingRemovals, (unsigned long long)Dropped);
	}

	// REPORT THE CEILING LOUDLY (§6.5.5: "a world at its ceiling is a world that
	// needs a re-bake, and that must be an operator-visible signal rather than a
	// silent stall"). Shares the 5 s throttle window with the active-budget
	// warning below because the two travel together in exactly the case that
	// matters -- a runaway breach trips both.
	if (Impl.Mob.atMobilizedCeiling() && (NowWorldSeconds - Impl.LastCeilingWarnWorldSeconds) > 5.0)
	{
		Impl.LastCeilingWarnWorldSeconds = NowWorldSeconds;
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("WaterMobilizer AT CEILING: mobilized=%llu >= voxel.Water.MobilizedCeilingBricks=%llu ")
		       TEXT("(front stalled; %llu ceiling refusals, %llu relief sweeps, %llu bricks demoted so far). ")
		       TEXT("The world is DEGRADED, not corrupt -- a refused brick is still a wall, so nothing leaks. ")
		       TEXT("This world wants a re-bake (§6.5.4); raising the cvar buys memory, not correctness."),
		       (unsigned long long)Impl.Mob.mobilizedBricks().size(),
		       (unsigned long long)Impl.Mob.mobilizedCeiling(),
		       (unsigned long long)Impl.Mob.ceilingRefusals(),
		       (unsigned long long)Impl.Mob.ceilingReliefCalls(),
		       (unsigned long long)Impl.Mob.demotedBrickCount());
	}

	// Task item 3: "if steppedBrickCount exceeds a cvar cap ... log-throttle
	// warning (do not explode)". The CA's tick contract (waterca.h) is
	// atomic over its whole active-set snapshot -- there is no safe mid-step
	// cutoff that wouldn't break volume conservation/determinism -- so this
	// is purely a monitoring signal, not a clamp.
	const int32 Cap = VoxelDebug::GetWaterMaxActiveBricks();
	if (Cap > 0 && Impl.LastSteppedBrickCount > Cap && (NowWorldSeconds - Impl.LastBudgetWarnWorldSeconds) > 5.0)
	{
		Impl.LastBudgetWarnWorldSeconds = NowWorldSeconds;
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("WaterCA over budget: steppedBrickCount=%d > voxel.Water.MaxActiveBricks=%d (tick ran in full -- ")
		       TEXT("no safe mid-step cutoff exists; raise the cap or investigate runaway spread)."),
		       Impl.LastSteppedBrickCount, Cap);
	}
}

// Server -> client replication (task item 1), fixed ~5Hz cadence,
// byte-capped per broadcast. No-op (and clears nothing) if there's nothing
// dirty this round.
void BroadcastWaterDiffs(FVoxelWaterImpl& Impl, UWorld& World, float DeltaTime)
{
	Impl.ReplicationAccumSeconds += DeltaTime;
	if (Impl.ReplicationAccumSeconds < FVoxelWaterImpl::ReplicationIntervalSeconds)
	{
		return;
	}
	Impl.ReplicationAccumSeconds = 0.f;

	// BOTH QUEUES GATE THIS, and the second one is easy to forget. A demotion
	// happens precisely when a brick has gone quiet and stopped producing fill
	// diffs, so the common case for a key removal is a round in which
	// DirtySinceLastBroadcast is EMPTY. Testing only the fill queue would have
	// dropped the removals silently and left every client holding bricks the
	// authority had already given back.
	// THREE QUEUES GATE THIS NOW. The basin queue is the Phase 2 addition and it
	// has the removals' problem in a sharper form: a lake rising is precisely a
	// change that produces NO dirty bricks (it is a scalar, not a CA cell), so
	// the common case for a basin row is a round in which both other queues are
	// empty. Testing only those two would have dropped every lake level change
	// on the floor and left the authority's lakes and the clients' lakes at
	// permanently different heights.
	if (Impl.DirtySinceLastBroadcast.Num() == 0 && Impl.PendingRemovals.empty() &&
	    Impl.BasinDirtySinceLastBroadcast.Num() == 0)
	{
		return;
	}

	std::vector<vxc::BrickKey> Keys;
	Keys.reserve(Impl.DirtySinceLastBroadcast.Num());
	for (const VoxelCoords::FVoxelCoord& C : Impl.DirtySinceLastBroadcast) Keys.push_back(ToBrickKey(C));

	// The basin rows, read LIVE from the ledger rather than snapshotted when the
	// basin was marked dirty. A basin credited five times in this window
	// replicates its CURRENT delta once, which is the whole reason the authority
	// is a level-independent volume: there is no history to replay, only a
	// number to agree on.
	//
	// SORTED BY KEY, so the payload is deterministic for a given set of moved
	// basins -- the TSet's own iteration order is not, and a wire format whose
	// bytes depend on hash order is one that cannot be diffed or golden-tested.
	std::vector<std::pair<uint64_t, int64_t>> BasinRows;
	if (Impl.Basins && Impl.BasinDirtySinceLastBroadcast.Num() > 0)
	{
		BasinRows.reserve(size_t(Impl.BasinDirtySinceLastBroadcast.Num()));
		for (uint64 Key : Impl.BasinDirtySinceLastBroadcast)
		{
			BasinRows.emplace_back(Key, Impl.Basins->deltaUnits(vxc::BasinId{Key}));
		}
		std::sort(BasinRows.begin(), BasinRows.end(),
		          [](const std::pair<uint64_t, int64_t>& A, const std::pair<uint64_t, int64_t>& B)
		          { return A.first < B.first; });
	}

	TArray<uint8> Bytes;
	int32 EncodedBrickCount = 0;
	size_t ConsumedKeyCount = 0;
	int32 EncodedRemovalCount = 0;
	size_t ConsumedRemovalCount = 0;
	int32 EncodedBasinCount = 0;
	size_t ConsumedBasinCount = 0;
	SerializeWaterDiffs(Keys, Impl.PendingRemovals, BasinRows, Impl.CA,
	                    FVoxelWaterImpl::MaxDiffBytesPerBroadcast, Bytes, EncodedBrickCount, ConsumedKeyCount,
	                    EncodedRemovalCount, ConsumedRemovalCount, EncodedBasinCount, ConsumedBasinCount);

	for (size_t i = 0; i < ConsumedKeyCount; ++i)
	{
		Impl.DirtySinceLastBroadcast.Remove(ToCoord(Keys[i]));
	}

	if (EncodedBrickCount == 0 && EncodedRemovalCount == 0 && EncodedBasinCount == 0)
	{
		return; // every dirty brick this round had already emptied again -- nothing to actually tell clients
	}

	if (ConsumedKeyCount < Keys.size())
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("BroadcastWaterDiffs: byte cap reached -- sent %d/%d dirty bricks this round (%d bytes); remainder ")
		       TEXT("deferred to next ~5Hz broadcast."),
		       EncodedBrickCount, (int32)Keys.size(), Bytes.Num());
	}

	AVoxelEditRelay* Relay = nullptr;
	for (TActorIterator<AVoxelEditRelay> It(&World); It; ++It)
	{
		Relay = *It;
		break;
	}
	if (!Relay)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("BroadcastWaterDiffs: no AVoxelEditRelay in the world -- %d brick diffs and %d key removals not ")
		       TEXT("broadcast."),
		       EncodedBrickCount, EncodedRemovalCount);
		return; // removals stay queued -- see below for why they must
	}

	Relay->MulticastWaterDiffs(Bytes);
	Impl.ReplicatedBytesThisWindow += Bytes.Num();

	// The basin rows are DROPPED ONLY ONCE THEY ARE ON THE WIRE, by the same
	// rule the removals below follow: a row that missed the cap is still a
	// disagreement between the authority's lake and the client's, and it stays
	// queued until it has actually been sent.
	for (size_t i = 0; i < ConsumedBasinCount && i < BasinRows.size(); ++i)
	{
		Impl.BasinDirtySinceLastBroadcast.Remove(BasinRows[i].first);
	}
	Impl.BasinReplicatedRows += int64(EncodedBasinCount);

	// DROP THE REMOVALS ONLY ONCE THEY ARE ACTUALLY ON THE WIRE. Fill diffs may
	// be dropped optimistically -- a brick that still matters is still dirty and
	// will be re-sent next round, so the queue is self-healing. A key removal has
	// no such property: `takeRecentlyDemoted()` handed it over exactly once and
	// nothing will ever regenerate it, so losing one here would leave clients
	// permanently holding a brick the authority has given back, with no symptom
	// until someone dug there. Hence: consume from the front, after the send, and
	// keep the remainder for the next round.
	Impl.PendingRemovals.erase(Impl.PendingRemovals.begin(),
	                           Impl.PendingRemovals.begin() + ptrdiff_t(ConsumedRemovalCount));
}
} // namespace

void UVoxelWaterSubsystem::Tick(float DeltaTime)
{
	if (!Impl)
	{
		return;
	}
	// docs/debug-tooling-plan.md P3 / task item 5c perf budget ("<2ms/frame
	// at v0 scale"): wall-clock this whole Tick() body, published unthrottled
	// (same convention as UVoxelWorldSubsystem::FVoxelPerfSnapshot::SubsystemTickMs).
	const double TickStartSeconds = FPlatformTime::Seconds();

	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;

	// Latch the authority answer where the pieces that must not run off it can
	// see it. Demotion (§6.5.3) is authority-only doctrine that voxel-core is
	// structurally unable to enforce -- it is netmode-free by design -- so the
	// enforcement lives at the engine call sites, and they read this.
	Impl->bAuthority = (NetMode != NM_Client);
	Impl->bReplicating = (World != nullptr && NetMode != NM_Client && NetMode != NM_Standalone);

	if (NetMode != NM_Client)
	{
		// W4 (ADR-0004): construct or tear down the shallow-water sheet to match
		// voxel.Water.SWE. Deliberately OUTSIDE the fixed-step loop below, so an
		// arm or a disarm always lands on a step boundary and never between the
		// coupler and the CA -- a grid appearing or vanishing mid-window would
		// break the conservation ledger's before/after pairing for that tick.
		// Cheap when nothing changed: two bool reads and a pointer compare.
		MaybeArmSwe(*Impl, World);

		// voxel.Water.ImplicitOcean, re-latched on a step boundary (§6.4).
		MaybeRelatchImplicitOcean(*Impl);

		// W3 (plan S3.7 Layer R): same placement, same reason -- an arm or a
		// disarm must not land between the river tick and the CA step.
		MaybeArmRivers(*Impl, World);

		Impl->TickAccumSeconds += DeltaTime;
		const double NowWorldSeconds = World ? World->GetTimeSeconds() : 0.0;
		int32 StepsThisFrame = 0;
		while (Impl->TickAccumSeconds >= FVoxelWaterImpl::FixedStepSeconds && StepsThisFrame < FVoxelWaterImpl::MaxStepsPerFrame)
		{
			Impl->TickAccumSeconds -= FVoxelWaterImpl::FixedStepSeconds;
			StepFixed(*Impl, NowWorldSeconds);
			++StepsThisFrame;
		}

		if (World && NetMode != NM_Standalone)
		{
			BroadcastWaterDiffs(*Impl, *World, DeltaTime);
		}
	}
	// NM_Client: authority-only simulation (task item 1) -- never steps its
	// own CA; rendering is kept current purely by ApplyReplicatedWaterDiffs.

	RemeshDirtyBricks(*Impl, ChunkOwner, ChunkRoot, WaterMaterial);

	// C7 static cavern lakes. Runs on clients too: the implicit field is pure
	// worldgen, so a client derives WHERE the water is straight from the seed
	// with no replication at all. What does have to replicate is which bricks
	// have MOBILIZED, because that is simulation state -- see
	// ApplyReplicatedWaterDiffs.
	// -VoxelWaterMarkerOnly=1 SUPPRESSES THE REAL WATER RENDERERS, so the marker
	// is the only thing on screen describing where water is.
	//
	// The owner's report, flying the marker 2026-08-06: "flying close to the
	// magenta water renders the blue water and ribbon which looks conflicting --
	// debugging and diagnosing this would be easier if we just showed magenta
	// cubic voxel blocks." Three renderers overlap inside 25.6 m -- the
	// near-field implicit disc here, the lake sheets and the river ribbons -- and
	// each draws the SAME baked water in a different colour, at a different
	// resolution, with a different extent rule. Judging the marker's geometry
	// through that is judging four things at once.
	//
	// The other two read their own -VoxelLakeSheets=0 / -VoxelRiverRibbons=0
	// inside their actors; this switch turns all three off together so the view
	// is one flag rather than three that can disagree.
	if (World && !VoxelWaterMarkerOnly())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector CameraUU = FVector::ZeroVector;
			FRotator UnusedRot = FRotator::ZeroRotator;
			PC->GetPlayerViewPoint(CameraUU, UnusedRot);
			RefreshImplicitWater(*Impl, CameraUU, ChunkOwner, ChunkRoot, WaterMaterial);
		}
	}

	// --- voxel.Water.GPU pool status, 1Hz -----------------------------------
	//
	// Budgeted before it was needed. A pooled primitive drawing thousands of
	// bricks has no component to select, no per-brick proxy to break on, and no
	// per-brick state of any kind -- so if the pooled water renders wrong or not
	// at all, this line and the pool's own "VoxelWaterPool[x,y,z] draw
	// SUBMITTED/SKIPPED" are the entire first rung of the diagnostic ladder.
	//
	// W5 ADDS THE BUCKET CENSUS, and it is the line to read first when the sort
	// A/B looks wrong. `buckets=1` means every water surface in the world is
	// still sharing one sort key and the split did not take effect -- which is
	// the pre-W5 picture exactly, and would otherwise be indistinguishable from
	// "the split worked and the sort was never the problem".
	// Deliberately mirrors the terrain pool's shape (live entries, quads,
	// free-vs-largest-run, which is the number that tells "genuinely full" from
	// "merely fragmented").
	Impl->PoolLogAccumSeconds += DeltaTime;
	if (Impl->PoolLogAccumSeconds >= 1.0f)
	{
		Impl->PoolLogAccumSeconds = 0.f;
		int32 LiveBuckets = 0;
		int32 DrawingBuckets = 0;
		int32 LiveEntries = 0;
		uint32 UsedQuads = 0;
		uint32 FreeQuads = 0;
		uint32 SmallestLargestRun = MAX_uint32;
		int32 MaxFreeRuns = 0;
		for (const FVoxelWaterImpl::FPoolBucket& Bucket : Impl->PoolBuckets)
		{
			const UVoxelGpuPoolComponent* Pool = Bucket.Pool.Get();
			if (Pool == nullptr)
			{
				continue;
			}
			++LiveBuckets;
			DrawingBuckets += (Pool->GetNumChunks() > 0) ? 1 : 0;
			LiveEntries += Pool->GetNumChunks();
			// GetNumQuads() is the pool's CAPACITY, not its occupancy -- the
			// whole buffer is allocated up front because incremental writes
			// address absolute offsets. Occupancy is capacity minus free.
			UsedQuads += uint32(Pool->GetNumQuads()) - Pool->GetFreeQuads();
			FreeQuads += Pool->GetFreeQuads();
			// The WORST bucket's largest free run, not the sum: fragmentation is
			// a per-allocator property and summing it across pools would report a
			// comfortable number for a set in which one pool is about to start
			// refusing geometry. Same argument for taking the max free-run count.
			SmallestLargestRun = FMath::Min(SmallestLargestRun, Pool->GetLargestFreeRun());
			MaxFreeRuns = FMath::Max(MaxFreeRuns, Pool->GetFreeRunCount());
		}
		if (LiveBuckets > 0)
		{
			UE_LOG(LogVoxelWater, Log,
			       TEXT("VoxelWaterPool: %d buckets (%d drawing, cap %d), %d live entries (%d CA + %d implicit), "
			            "%u quads used, %u free, worst largest run %u, worst free runs %d"),
			       LiveBuckets, DrawingBuckets, kMaxWaterPoolBuckets, LiveEntries, Impl->PoolSlots.Num(),
			       Impl->ImplicitPoolSlots.Num(), UsedQuads, FreeQuads, SmallestLargestRun, MaxFreeRuns);
		}
	}

	// --- Perf snapshot (task item 3), 1Hz -----------------------------------
	Impl->PerfRefreshAccumSeconds += DeltaTime;
	if (Impl->PerfRefreshAccumSeconds >= 1.0f)
	{
		const float Window = Impl->PerfRefreshAccumSeconds;
		Impl->PerfRefreshAccumSeconds = 0.f;

		FVoxelWaterPerfSnapshot Snap;
		Snap.ActiveBricks = int64(Impl->CA.activeBrickCount());
		Snap.StoredBricks = int64(Impl->CA.storedBrickCount());
		Snap.TotalVolume = Impl->CA.totalVolume();
		Snap.StepsPerSec = float(Impl->StepsThisWindow) / Window;
		Snap.LastSteppedBrickCount = Impl->LastSteppedBrickCount;
		Snap.ReplicatedBytesPerSec = float(Impl->ReplicatedBytesThisWindow) / Window;
		// Always 0 since §6.4 retired Reservoir v0. Reported rather than
		// removed so the snapshot struct, the HUD and every parsed log line
		// keep their shape; a field that reads 0 forever is a smaller change to
		// every consumer than a field that disappears mid-milestone. It goes
		// when the perf snapshot is next revised.
		Snap.ReservoirCells = 0;

		Snap.TickMs = Impl->LastSnapshot.TickMs; // preserve the always-fresh field across this reassignment
		Impl->LastSnapshot = Snap;
		Impl->StepsThisWindow = 0;
		Impl->ReplicatedBytesThisWindow = 0;

		// Task item 5c perf-budget evidence ("water tick must stay <2ms/frame
		// at v0 scale; report actual"): a 1Hz log-line trail while water is
		// actually active, readable alongside -VoxelPerfRun's own p50/p95
		// frame-time JSON summary. Skipped while the sim is fully idle (no
		// stored bricks, zero volume) so a normal no-water run stays quiet.
		if (Snap.StoredBricks > 0 || Snap.TotalVolume > 0)
		{
			UE_LOG(LogVoxelPerf, Log,
			       TEXT("WaterPerf: activeBricks=%lld stored=%lld volume=%llu steps/s=%.1f tickMs=%.3f replKB/s=%.2f"),
			       Snap.ActiveBricks, Snap.StoredBricks, (unsigned long long)Snap.TotalVolume, Snap.StepsPerSec, Snap.TickMs,
			       Snap.ReplicatedBytesPerSec / 1024.0);
		}
	}

	Impl->LastSnapshot.TickMs = float((FPlatformTime::Seconds() - TickStartSeconds) * 1000.0);
}

TStatId UVoxelWaterSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelWaterSubsystem, STATGROUP_Tickables);
}

uint32 UVoxelWaterSubsystem::SpawnWaterAt(const FVector& WorldLocationUU, uint32 Amount)
{
	if (!Impl)
	{
		return 0;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("SpawnWaterAt refused on NM_Client -- water sim is authority-only (task item 1)."));
		return 0;
	}

	const VoxelCoords::FVoxelCoord Vc = VoxelCoords::WorldToVoxel(WorldLocationUU);
	const uint32 Placed = Impl->CA.addWater(Vc.X, Vc.Y, Vc.Z, Amount);
	if (Placed > 0)
	{
		MarkColumnDirty(*Impl, Vc.X, Vc.Y, Vc.Z, Placed);
		UE_LOG(LogVoxelWater, Log, TEXT("SpawnWaterAt: placed %u/%u fill units at voxel (%lld,%lld,%lld)"), Placed, Amount,
		       (long long)Vc.X, (long long)Vc.Y, (long long)Vc.Z);
	}
	return Placed;
}

void UVoxelWaterSubsystem::NotifyTerrainVoxelsCleared(const TArray<VoxelCoords::FVoxelCoord>& ClearedVoxels)
{
	if (!Impl || ClearedVoxels.Num() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		return; // simulation state, authority-only (same rule as the CA tick)
	}

	// THIS FUNCTION NO LONGER SEEDS ANYTHING (watershed plan §6.4, work item 8).
	// It used to run the Reservoir v0 rule; the top-up loop it fed is gone from
	// StepFixed, and that block carries the measurements that justify the
	// deletion. The sea is now a term of the implicit field, so a dig into it
	// releases water through the SAME funnel a dug lake shore uses --
	// NotifyTerrainRegionEdited -> WaterMobilizer::mobilizeEditRegion -- which
	// UVoxelWorldSubsystem already calls beside every one of the five call
	// sites that call this one. Nothing here has to make water happen.
	//
	// WHAT IS LEFT IS THE LOG LINE, and it is kept rather than deleted because
	// it is the ONLY signal an unattended headless run has that a dig actually
	// opened the sea. The old line reported "seeded N boundary cells", which
	// was a count of a mechanism; this one reports the DATUM TEST -- how many
	// cleared voxels stand below sea level in a column whose WORLDGEN ground is
	// also below it, i.e. genuinely in the sea, versus how many are merely
	// below z=0 inside land, which is the inland pit the old rule flooded.
	// Those two numbers side by side are exactly what the §6.4 inland-pit
	// capture needs to read.
	//
	// COST: one amplifier column per distinct COLUMN below the datum, not per
	// voxel. GroundMmAt memoises one column and GetSurfaceHeightUU also fires a
	// fine-tile streaming request, so a per-voxel query on a carve would put
	// thousands of them on the game thread; the dedupe below is what keeps this
	// diagnostic from costing more than the mechanism it replaced.
	TSet<uint64> ColumnsSeen;
	int32 SeaVoxels = 0;
	int32 InlandBelowDatumVoxels = 0;
	for (const VoxelCoords::FVoxelCoord& V : ClearedVoxels)
	{
		if (V.Z >= vxc::kSeaLevelVoxelZ)
		{
			continue; // above the datum: never sea, whatever the column says
		}
		const int32_t GroundMm = Impl->GroundMmAt(V.X, V.Y);
		if (vxc::oceanSurfaceMmAt(GroundMm) == vxc::kNoWaterMm)
		{
			++InlandBelowDatumVoxels; // a pit dug into land -- and it stays dry
			continue;
		}
		++SeaVoxels;
		ColumnsSeen.Add((uint64(uint32(V.X)) << 32) | uint64(uint32(V.Y)));
	}

	if (SeaVoxels > 0 || InlandBelowDatumVoxels > 0)
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("Dig below the datum: %d cleared voxel(s) in %d seabed column(s) are IN THE SEA "
		            "(worldgen ground below kSeaLevelMm) and mobilize through the implicit field; %d "
		            "are inland pit (ground above the datum) and stay dry. %d cleared voxel(s) "
		            "examined; Reservoir v0 retired, see StepFixed."),
		       SeaVoxels, ColumnsSeen.Num(), InlandBelowDatumVoxels, ClearedVoxels.Num());
	}
}

void UVoxelWaterSubsystem::NotifyTerrainRegionEdited(const VoxelCoords::FVoxelCoord& MinVoxelIncl,
                                                       const VoxelCoords::FVoxelCoord& MaxVoxelIncl)
{
	if (!Impl)
	{
		return;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		return; // a client's local mirror never runs step()/hydrostaticPass -- nothing to invalidate
	}
	Impl->CA.invalidateSolidRegion(MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X, MaxVoxelIncl.Y,
	                               MaxVoxelIncl.Z);

	// W4 (ADR-0004): the CA's memo is not the only thing an edit invalidates.
	// The sheet's beds were seated once, when the cvar armed, and swe.h S5(a)
	// deliberately never moves one -- so an edit that takes the ground out from
	// under a sheet column leaves it resting on air, which eligible() rejects
	// forever. Queue the columns in this edit's own footprint whose bed has
	// actually gone; ReseatEditedSweBeds drains the queue inside the coupled
	// window, and its comment carries the whole argument. Costs one solidity
	// query per edited column, on the edit, and nothing at all when the sheet is
	// not armed -- which is every run that has not set voxel.Water.SWE.
	if (Impl->SweSheet && Impl->SweCoupler)
	{
		const vxc::SweGrid& Grid = *Impl->SweSheet;
		const vxc::WaterCA::SolidFn Solid = Impl->Mob.makeSolidFn();
		const int64 X0 = FMath::Max<int64>(MinVoxelIncl.X, Grid.originVx());
		const int64 X1 = FMath::Min<int64>(MaxVoxelIncl.X, Grid.originVx() + Grid.sizeX() - 1);
		const int64 Y0 = FMath::Max<int64>(MinVoxelIncl.Y, Grid.originVy());
		const int64 Y1 = FMath::Min<int64>(MaxVoxelIncl.Y, Grid.originVy() + Grid.sizeY() - 1);
		for (int64 Vy = Y0; Vy <= Y1; ++Vy)
		{
			for (int64 Vx = X0; Vx <= X1; ++Vx)
			{
				if (Solid(Vx, Vy, Grid.bedAt(Vx, Vy)) != vxc::MAT_AIR)
				{
					continue;
				}
				Impl->SwePendingBedReseat.Add(
					int32((Vx - Grid.originVx()) + int64(Grid.sizeX()) * (Vy - Grid.originVy())));
			}
		}
	}

	// ...and then actually WAKE the water this edit can affect. The memo
	// invalidation above only fixes what the CA believes about terrain; without
	// this call a settled body never ticks again and simply ignores the edit
	// (see the header comment on this function, and voxelcore/waterca.h
	// "Terrain-edit reactivation"). Scheduling only: no fill is written, so the
	// conservation ledger is untouched. Water-bearing bricks only, within one
	// brick of the edit -- an edit nowhere near water costs one bounded probe
	// and wakes nothing.
	// C8 mobilize-on-approach, THE SEED (docs/cavern-design.md SS5.2). This has
	// to happen here and not only in the CA-activity front, because digging into
	// the wall of a static lake produces no CA activity whatsoever -- there is no
	// CA water yet -- so without an edit hook the advancing front would have
	// nothing to grow from and the player would face a frozen wall of water.
	// Costs a bounded scan of the edit's own bricks when they hold no implicit
	// water, which is every edit outside a flooded cavern.
	const size_t Converted = Impl->Mob.mobilizeEditRegion(Impl->CA, MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z,
	                                                      MaxVoxelIncl.X, MaxVoxelIncl.Y, MaxVoxelIncl.Z);
	if (Converted > 0)
	{
		MarkMobilizedBricksDirty(*Impl);
		UE_LOG(LogVoxelWater, Log,
		       TEXT("Mobilized %d cavern water brick(s) on edit [%d,%d,%d]..[%d,%d,%d] (implicit -> CA; ledger %llu debited / %llu credited, shortfall %llu)"),
		       static_cast<int32>(Converted), MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X,
		       MaxVoxelIncl.Y, MaxVoxelIncl.Z, (unsigned long long)Impl->Mob.debitedVolume(),
		       (unsigned long long)Impl->Mob.creditedVolume(), (unsigned long long)Impl->Mob.shortfallVolume());
	}

	const size_t Woken = Impl->CA.wakeRegion(MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X,
	                                         MaxVoxelIncl.Y, MaxVoxelIncl.Z);
	if (Woken > 0)
	{
		UE_LOG(LogVoxelWater, Verbose,
		       TEXT("NotifyTerrainRegionEdited: woke %d water brick(s) for edit [%d,%d,%d]..[%d,%d,%d]"),
		       static_cast<int32>(Woken), MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X,
		       MaxVoxelIncl.Y, MaxVoxelIncl.Z);
	}
}

bool UVoxelWaterSubsystem::IsSolidCacheEnabled() const
{
	return Impl && Impl->CA.solidCacheEnabled();
}

FVoxelWaterPerfSnapshot UVoxelWaterSubsystem::GetPerfSnapshot() const
{
	return Impl ? Impl->LastSnapshot : FVoxelWaterPerfSnapshot{};
}

uint64 UVoxelWaterSubsystem::GetWaterDigest() const
{
	if (!Impl)
	{
		return 0;
	}
	vxc::Digest D;
	Impl->CA.digest(D);
	return D.h;
}

bool UVoxelWaterSubsystem::GetStoredWaterCentroidUU(FVector& OutCenterUU) const
{
	if (!Impl || Impl->CA.storedBrickCount() == 0)
	{
		return false;
	}
	double SumX = 0.0, SumY = 0.0, SumZ = 0.0;
	int64 Count = 0;
	for (const auto& Entry : Impl->CA.bricks())
	{
		const vxc::BrickKey& Key = Entry.first;
		// Brick-center world position (UU): brick origin + half an edge.
		SumX += (double(Key.x) + 0.5) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
		SumY += (double(Key.y) + 0.5) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
		SumZ += (double(Key.z) + 0.5) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
		++Count;
	}
	OutCenterUU = FVector(SumX / double(Count), SumY / double(Count), SumZ / double(Count));
	return true;
}

uint8 UVoxelWaterSubsystem::GetMaxStoredFill() const
{
	if (!Impl)
	{
		return 0;
	}
	uint8_t Max = 0;
	for (const auto& Entry : Impl->CA.bricks())
	{
		const vxc::WaterBrick8& B = Entry.second;
		for (int z = 0; z < vxc::WaterBrick8::kEdge; ++z)
			for (int y = 0; y < vxc::WaterBrick8::kEdge; ++y)
				for (int x = 0; x < vxc::WaterBrick8::kEdge; ++x) Max = FMath::Max(Max, B.get(x, y, z));
	}
	return Max;
}

// --- W4 SWE diagnostics (VoxelWaterSubsystem.h's three PODs) -----------------
//
// Read-only, added for -VoxelSweBreachTest. See the header for why they exist
// at all; the short version is that the sheet lives inside FVoxelWaterImpl and
// the difference between "spreading correctly" and "seated wrong" is a
// per-column comparison, not a total.

bool UVoxelWaterSubsystem::GetSweProbe(FVoxelSweProbe& Out) const
{
	Out = FVoxelSweProbe{};
	if (!Impl)
	{
		return false;
	}
	// CA volume is reported whether or not the sheet is armed -- a CA-only run
	// of the fixture needs exactly this number to compare against.
	Out.CaVolume = int64(Impl->CA.totalVolume());
	if (!Impl->SweSheet || !Impl->SweCoupler)
	{
		return false;
	}

	const vxc::SweGrid& Grid = *Impl->SweSheet;
	Out.bArmed = true;
	Out.SheetVolume = Grid.totalVolume();
	Out.Injected = Impl->SweInjected;
	Out.ConservationFailures = Impl->SweConservationFailures;
	Out.SweColumns = Impl->SweCoupler->sweColumnCount();
	Out.LastPunctured = Impl->SweCoupler->lastPuncturedCount();
	Out.TransferredToCA = Impl->SweCoupler->transferredToCA();
	Out.TransferredToSWE = Impl->SweCoupler->transferredToSWE();
	Out.BedsReseated = Impl->SweBedsReseated;
	Out.PendingBedReseats = Impl->SwePendingBedReseat.Num();
	Out.OriginVx = Grid.originVx();
	Out.OriginVy = Grid.originVy();
	Out.SizeX = Grid.sizeX();
	Out.SizeY = Grid.sizeY();
	Out.SheetScanVoxels = Impl->SweCoupler->config().sheetScanVoxels;
	Out.SettleToleranceFill = vxc::sweSettleTolerance(Grid.config());
	return true;
}

bool UVoxelWaterSubsystem::GetWaterColumnProbe(int64 Vx, int64 Vy, int64 ScanFromVz, int32 ScanVoxels,
                                                FVoxelWaterColumnProbe& Out) const
{
	Out = FVoxelWaterColumnProbe{};
	Out.Vx = Vx;
	Out.Vy = Vy;
	if (!Impl)
	{
		return false;
	}
	ScanVoxels = FMath::Max(ScanVoxels, 1);

	// CA side and terrain side in ONE downward walk, so the two answers are
	// read from the same instant and the same window.
	for (int32 K = 0; K < ScanVoxels; ++K)
	{
		const int64 Vz = ScanFromVz - K;
		const uint8 Fill = Impl->CA.fillAt(Vx, Vy, Vz);
		if (Fill != 0)
		{
			Out.CaColumnFill += int32(Fill);
			if (Out.CaTopFill == 0)
			{
				Out.CaTopFill = Fill;
				Out.CaTopVz = Vz;
			}
		}
		if (!Out.bTerrainTopFound && Impl->Terrain.IsSolidAtVoxel(Vx, Vy, Vz))
		{
			Out.bTerrainTopFound = true;
			Out.TerrainTopSolidVz = Vz;
		}
	}

	if (!Impl->SweSheet || !Impl->SweCoupler)
	{
		return true;
	}
	const vxc::SweGrid& Grid = *Impl->SweSheet;
	Out.bSweArmed = true;
	if (!Grid.inBounds(Vx, Vy))
	{
		return true;
	}
	Out.bInSheet = true;
	Out.bSweOwned = Impl->SweCoupler->isSweColumn(Vx, Vy);
	Out.Bed = Grid.bedAt(Vx, Vy);
	Out.Depth = Grid.depthAt(Vx, Vy);
	const vxc::SweVelocity V = Grid.velocityAt(Vx, Vy);
	Out.VelXMmPerSec = V.xMmPerSec;
	Out.VelYMmPerSec = V.yMmPerSec;
	Out.FluxXFillPerTick = Grid.faceFluxX(Vx, Vy);
	Out.FluxYFillPerTick = Grid.faceFluxY(Vx, Vy);
	return true;
}

bool UVoxelWaterSubsystem::AuditSweBedSeating(FVoxelSweBedAudit& Out) const
{
	Out = FVoxelSweBedAudit{};
	if (!Impl || !Impl->SweSheet || !Impl->SweCoupler)
	{
		return false;
	}
	Out.bArmed = true;

	const vxc::SweGrid& Grid = *Impl->SweSheet;

	// THE SAME callback the arming path built and handed to both SeatSweBedZ
	// and the coupler. Re-deriving it here (rather than using the bare terrain
	// query) is the whole point: the audit must ask the question the seating
	// code asked, or a "mismatch" would just be the mobilizer wrapper.
	vxc::WaterCA::SolidFn Solid = Impl->Mob.makeSolidFn();

	for (int32 Cy = 0; Cy < Grid.sizeY(); ++Cy)
	{
		for (int32 Cx = 0; Cx < Grid.sizeX(); ++Cx)
		{
			const int64 Vx = Grid.originVx() + Cx;
			const int64 Vy = Grid.originVy() + Cy;
			++Out.Columns;

			const int32 StoredBed = Grid.bedAt(Vx, Vy);
			const bool bSweOwned = Impl->SweCoupler->isSweColumn(Vx, Vy);
			if (bSweOwned)
			{
				++Out.SweOwned;
			}
			if (Solid(Vx, Vy, StoredBed) != vxc::MAT_AIR)
			{
				++Out.Seated;
			}

			// (a) Re-seat with the identical function and the live world.
			const double SurfaceZUU = Impl->Terrain.GetSurfaceHeightUU(double(Vx) * VoxelCoords::VoxelSizeUU,
			                                                           double(Vy) * VoxelCoords::VoxelSizeUU);
			const int64 SurfaceVz = int64(FMath::FloorToDouble(SurfaceZUU / VoxelCoords::VoxelSizeUU));
			bool bFound = false;
			const int32 Reseated = SeatSweBedZ(Solid, Vx, Vy, SurfaceVz, bFound);
			if (bFound && Reseated != StoredBed)
			{
				++Out.Mismatched;
				if (bSweOwned)
				{
					++Out.SweOwnedMismatched;
				}
				const int32 AbsDelta = FMath::Abs(Reseated - StoredBed);
				Out.SumAbsDeltaVoxels += AbsDelta;
				if (AbsDelta > Out.MaxAbsDeltaVoxels)
				{
					Out.MaxAbsDeltaVoxels = AbsDelta;
					Out.WorstVx = Vx;
					Out.WorstVy = Vy;
					Out.WorstStoredBed = StoredBed;
					Out.WorstReseatedBed = Reseated;
				}
			}

			// (b) The bare-terrain cross-check, over the same window, with no
			// mobilizer wrapper. Differences here are EXPECTED over implicit
			// cavern water and are reported separately for that reason.
			const int64 TopZ = SurfaceVz + kSweBedScanAboveVoxels;
			bool bTerrainFound = false;
			int64 TerrainTop = 0;
			for (int32 K = 0; K < kSweBedScanVoxels + kSweBedScanAboveVoxels; ++K)
			{
				if (Impl->Terrain.IsSolidAtVoxel(Vx, Vy, TopZ - K))
				{
					bTerrainFound = true;
					TerrainTop = TopZ - K;
					break;
				}
			}
			if (bTerrainFound && TerrainTop != int64(StoredBed))
			{
				++Out.MismatchedVsTerrain;
			}
		}
	}
	return true;
}

// --- C7/C8 underground water ------------------------------------------------

bool UVoxelWaterSubsystem::GetCavernFloodZUU(double WorldXUU, double WorldYUU, double& OutFloodZUU) const
{
	if (!Impl)
	{
		return false;
	}
	const int64 Vx = int64(FMath::FloorToDouble(WorldXUU / VoxelCoords::VoxelSizeUU));
	const int64 Vy = int64(FMath::FloorToDouble(WorldYUU / VoxelCoords::VoxelSizeUU));
	const int32 FloodZMm = Impl->Amp.columnCached(Vx, Vy).cavern.floodZMm;
	if (FloodZMm == INT32_MIN)
	{
		return false;
	}
	OutFloodZUU = double(FloodZMm) / 10.0; // mm -> UU (1 UU = 10 mm)
	return true;
}

bool UVoxelWaterSubsystem::FindFloodedCavernNear(const FVector& OriginUU, double SearchRadiusUU,
                                                  FVector& OutWaterSurfaceUU) const
{
	if (!Impl)
	{
		return false;
	}
	// 30 m grid: under a cavern site's ~36 m reach radius, so a site inside the
	// search radius cannot be stepped over.
	constexpr double StepUU = 3000.0;
	const int32 Steps = FMath::Max(1, int32(SearchRadiusUU / StepUU));

	// Contiguous open voxels spanning the waterline at this column: how much
	// lake there is below and how much headroom above. This, not a two-point
	// probe, is what tells the middle of a chamber from its tapering edge.
	constexpr int32 kProbeVoxels = 60; // +/- 6 m
	constexpr int32 kMinAirSpanVoxels = 40; // 4 m of open water + headroom
	// Scored as 2*min(headroom, depth) rather than the plain sum: a column with
	// 6 m of water and no roof clearance scores the same as one with 6 m of
	// roof and no water under the plain sum, and only the BALANCED one gives a
	// camera both a lake to look at and somewhere to stand.
	const auto AirSpanAround = [this](int64 Vx, int64 Vy, int64 Vz) -> int32
	{
		int32 Above = 0, Below = 0;
		for (int32 D = 0; D < kProbeVoxels; ++D)
		{
			if (Impl->Terrain.IsSolidAtVoxel(Vx, Vy, Vz + D)) break;
			++Above;
		}
		for (int32 D = 1; D < kProbeVoxels; ++D)
		{
			if (Impl->Terrain.IsSolidAtVoxel(Vx, Vy, Vz - D)) break;
			++Below;
		}
		return 2 * FMath::Min(Above, Below);
	};

	// Expanding rings, so the NEAREST flooded cavern wins rather than whichever
	// one a raster scan happens to reach first.
	for (int32 Ring = 0; Ring <= Steps; ++Ring)
	{
		for (int32 Iy = -Ring; Iy <= Ring; ++Iy)
		{
			for (int32 Ix = -Ring; Ix <= Ring; ++Ix)
			{
				if (Ring > 0 && FMath::Abs(Ix) != Ring && FMath::Abs(Iy) != Ring)
				{
					continue; // interior of the ring: already tested on an earlier ring
				}
				const double Wx = OriginUU.X + double(Ix) * StepUU;
				const double Wy = OriginUU.Y + double(Iy) * StepUU;
				const int64 Vx = int64(FMath::FloorToDouble(Wx / VoxelCoords::VoxelSizeUU));
				const int64 Vy = int64(FMath::FloorToDouble(Wy / VoxelCoords::VoxelSizeUU));

				// BY VALUE, not by reference. Amplifier::columnCached returns a
				// reference into a per-thread memo that the NEXT columnCached
				// call overwrites, and the refinement loop below makes hundreds
				// of them -- reading Col afterwards is a dangling read (it was
				// silently reporting the wrong cavern seg count before this).
				const vxc::ColumnSample Col = Impl->Amp.columnCached(Vx, Vy);
				if (Col.cavern.floodZMm == INT32_MIN || Col.cavern.count == 0)
				{
					continue; // dry, or no room actually reaches this column
				}

				// A column can carry a site's flood level while its own rooms are
				// truncated away by the roof/bedrock clamps, so require the
				// column to be genuinely open here: water below the flood level
				// and real headroom above it.
				const int64 FloodVz = int64(Col.cavern.floodZMm) / vxc::kVoxelSizeMm;
				if (AirSpanAround(Vx, Vy, FloodVz) < kMinAirSpanVoxels)
				{
					continue;
				}

				// REFINE. The search grid is 30 m and a room is 24-56 m across,
				// so the first hit is usually somewhere on the reach disc's edge,
				// where the roof closes down and a camera placed "just above the
				// water" ends up inside rock -- which renders as a see-through
				// world, because the surrounding geometry is all backfaces.
				// Walk a local grid and take the column with the most open air
				// around the waterline, i.e. the middle of the chamber.
				double BestWx = Wx, BestWy = Wy;
				int32 BestSpan = AirSpanAround(Vx, Vy, FloodVz);
				constexpr double RefineStepUU = 200.0; // 2 m
				constexpr int32 RefineSteps = 12;      // +/- 24 m
				for (int32 Ry = -RefineSteps; Ry <= RefineSteps; ++Ry)
				{
					for (int32 Rx = -RefineSteps; Rx <= RefineSteps; ++Rx)
					{
						const double Cx = Wx + double(Rx) * RefineStepUU;
						const double Cy = Wy + double(Ry) * RefineStepUU;
						const int64 Cvx = int64(FMath::FloorToDouble(Cx / VoxelCoords::VoxelSizeUU));
						const int64 Cvy = int64(FMath::FloorToDouble(Cy / VoxelCoords::VoxelSizeUU));
						// Must belong to the SAME lake, not a neighbouring site.
						if (Impl->Amp.columnCached(Cvx, Cvy).cavern.floodZMm != Col.cavern.floodZMm)
						{
							continue;
						}
						const int32 Span = AirSpanAround(Cvx, Cvy, FloodVz);
						if (Span > BestSpan)
						{
							BestSpan = Span;
							BestWx = Cx;
							BestWy = Cy;
						}
					}
				}

				OutWaterSurfaceUU = FVector(BestWx, BestWy, double(Col.cavern.floodZMm) / 10.0);
				UE_LOG(LogVoxelWater, Log,
				       TEXT("FindFloodedCavernNear: flooded cavern at (%.0f, %.0f) UU, water surface z=%.0f UU (floodZ %d mm), %d seg(s), open air span %d voxels (%.1f m) around the waterline"),
				       BestWx, BestWy, OutWaterSurfaceUU.Z, Col.cavern.floodZMm, Col.cavern.count, BestSpan,
				       double(BestSpan) * VoxelCoords::VoxelSizeUU / 100.0);
				return true;
			}
		}
	}
	UE_LOG(LogVoxelWater, Warning, TEXT("FindFloodedCavernNear: no flooded cavern within %.0f UU of (%.0f, %.0f)."),
	       SearchRadiusUU, OriginUU.X, OriginUU.Y);
	return false;
}

int32 UVoxelWaterSubsystem::CarveCavernOutflow(const FVector& LakeSurfaceUU)
{
	if (!Impl)
	{
		return 0;
	}

	// Walk +X until the flood field runs out -- that is the edge of this site's
	// reach disc, and the first place water can actually go.
	constexpr double ProbeStepUU = 200.0;  // 2 m
	constexpr double MaxProbeUU = 8000.0;  // 80 m, comfortably past a site's reach
	double ExitX = LakeSurfaceUU.X;
	bool bFoundExit = false;
	for (double D = ProbeStepUU; D <= MaxProbeUU; D += ProbeStepUU)
	{
		double Unused = 0.0;
		if (!GetCavernFloodZUU(LakeSurfaceUU.X + D, LakeSurfaceUU.Y, Unused))
		{
			ExitX = LakeSurfaceUU.X + D;
			bFoundExit = true;
			break;
		}
	}
	if (!bFoundExit)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("CarveCavernOutflow: no dry column within %.0f UU -- lake has nowhere to drain."), MaxProbeUU);
		return 0;
	}

	// A tunnel just under the water surface, so it takes the top of the lake
	// and the level visibly drops, then a shaft down past the dry edge so the
	// water keeps running away instead of backing up.
	constexpr double TunnelRadiusUU = 250.0; // 2.5 m
	constexpr double StepUU = 150.0;         // 1.5 m, well under the radius: no gaps
	const double TunnelZ = LakeSurfaceUU.Z - 100.0; // 1 m below the surface

	int32 Removed = 0;
	int32 Spheres = 0, ProductiveSpheres = 0;
	for (double X = LakeSurfaceUU.X; X <= ExitX + 400.0; X += StepUU)
	{
		// Three stacked passes rather than one: a single-radius tunnel at a
		// fixed z can thread a chamber that is open at that exact height for
		// its whole length and remove NOTHING, which is a silent no-drain.
		// Spanning 5 m vertically guarantees it meets the chamber's rock.
		for (double DZ = -200.0; DZ <= 200.0; DZ += 200.0)
		{
			const int32 R = Impl->Terrain.CarveSphere(FVector(X, LakeSurfaceUU.Y, TunnelZ + DZ), TunnelRadiusUU, 0.0);
			++Spheres;
			if (R > 0) ++ProductiveSpheres;
			Removed += R;
		}
	}
	// Drop shaft at the dry end.
	for (double Z = TunnelZ; Z >= TunnelZ - 1500.0; Z -= StepUU)
	{
		Removed += Impl->Terrain.CarveSphere(FVector(ExitX + 400.0, LakeSurfaceUU.Y, Z), TunnelRadiusUU, 0.0);
	}

	int32 MobBricks = 0;
	uint64 Deb = 0, Cred = 0, Short = 0;
	GetMobilizationStats(MobBricks, Deb, Cred, Short);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("CarveCavernOutflow: removed %d voxels via %d/%d productive sphere(s), exit at x=%.0f UU. Mobilized %d brick(s), ledger %llu debited / %llu credited, shortfall %llu."),
	       Removed, ProductiveSpheres, Spheres, ExitX, MobBricks, (unsigned long long)Deb, (unsigned long long)Cred,
	       (unsigned long long)Short);
	if (Removed == 0)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("CarveCavernOutflow: carved nothing -- every target cell was already air, so no edit fired and nothing mobilized."));
	}
	return Removed;
}

uint8 UVoxelWaterSubsystem::GetImplicitFillAtWorld(const FVector& WorldUU) const
{
	if (!Impl)
	{
		return 0;
	}
	return Impl->Mob.implicitFillAt(int64(FMath::FloorToDouble(WorldUU.X / VoxelCoords::VoxelSizeUU)),
	                                 int64(FMath::FloorToDouble(WorldUU.Y / VoxelCoords::VoxelSizeUU)),
	                                 int64(FMath::FloorToDouble(WorldUU.Z / VoxelCoords::VoxelSizeUU)));
}

uint8 UVoxelWaterSubsystem::GetWaterFillAtWorld(const FVector& WorldUU) const
{
	if (!Impl)
	{
		return 0;
	}
	const int64 Vx = int64(FMath::FloorToDouble(WorldUU.X / VoxelCoords::VoxelSizeUU));
	const int64 Vy = int64(FMath::FloorToDouble(WorldUU.Y / VoxelCoords::VoxelSizeUU));
	const int64 Vz = int64(FMath::FloorToDouble(WorldUU.Z / VoxelCoords::VoxelSizeUU));
	// CA first: once a brick is mobilized the CA owns it and implicitFillAt
	// already reads 0 there, so the max of the two is the whole water column
	// with no double count in either direction (waterca.h's ownership
	// partition is what guarantees that, not call order here).
	const uint8 CaFill = Impl->CA.fillAt(Vx, Vy, Vz);
	return CaFill > 0 ? CaFill : Impl->Mob.implicitFillAt(Vx, Vy, Vz);
}

double UVoxelWaterSubsystem::SeaLevelZUU()
{
	// mm -> UU. VoxelCoords::VoxelSizeUU UU per vxc::kVoxelSizeMm mm.
	return double(vxc::kSeaLevelMm) * VoxelCoords::VoxelSizeUU / double(vxc::kVoxelSizeMm);
}

bool UVoxelWaterSubsystem::IsOpenSeaAtWorld(double WorldZUU, double WorldgenGroundZUU)
{
	const double SeaZ = SeaLevelZUU();
	// Above the datum is never sea; below the seabed is rock, not sea.
	return WorldZUU < SeaZ && WorldgenGroundZUU < SeaZ && WorldZUU >= WorldgenGroundZUU;
}

bool UVoxelWaterSubsystem::IsUnderwaterAtWorld(const FVector& WorldUU) const
{
	if (GetWaterFillAtWorld(WorldUU) > 0)
	{
		return true;
	}
	if (!Impl)
	{
		return false;
	}
	// GetSurfaceHeightUU is the AMPLIFIER column surface -- worldgen, not the
	// edited overlay -- which is exactly what the seabed test needs: a pit a
	// player dug into land is not an ocean, and its edited ground is.
	return IsOpenSeaAtWorld(WorldUU.Z, Impl->Terrain.GetSurfaceHeightUU(WorldUU.X, WorldUU.Y));
}

// --- LAKE SHEETS AT RANGE (watershed plan item 5, §5.2) ---------------------
//
// All three of these are thin: the registry, the extent rule and the
// decomposition all live in voxelcore/lakes.h, and what is here is the world-UU
// arithmetic plus the tile enumeration. That split is the point -- the geometry
// is unit-tested in ctest without an engine, and the engine half cannot express
// a different lake shape than the near field's because it reads the same masks
// through the same sampler.

void UVoxelWaterSubsystem::FineTileForWorldUU(double XUU, double YUU, int32& OutTileX, int32& OutTileY)
{
	const vxc::TileCoord T =
		FVoxelFineTileStreamer::CoarseTileForWorldMm(VoxelCoords::WorldToMm(XUU), VoxelCoords::WorldToMm(YUU));
	OutTileX = T.x;
	OutTileY = T.y;
}

int32 UVoxelWaterSubsystem::GatherLakeSheetBasinsInTile(int32 TileX, int32 TileY, double CenterXUU,
                                                        double CenterYUU, double RadiusUU,
                                                        TArray<FLakeSheetBasin>& Out) const
{
	if (!Impl || !Impl->Water)
	{
		return 0;
	}
	check(IsInGameThread()); // loads a tile file; see the header

	const std::vector<vxc::BasinEntry>* Basins = Impl->Water->basinsForTile(TileX, TileY);
	if (Basins == nullptr)
	{
		return 0; // no fine tier, tile absent, or baked before the registry
	}
	// Read AFTER the tile is resolved: both are 0 until one has loaded.
	const int64 TileSize = int64(Impl->Water->tilePixels());
	const int64 PixelMm = int64(Impl->Water->pixelSizeMm());
	if (TileSize <= 0 || PixelMm <= 0)
	{
		return 0;
	}

	const int64 OxPx = int64(TileX) * TileSize, OyPx = int64(TileY) * TileSize;
	const int32 Before = Out.Num();
	for (size_t i = 0; i < Basins->size(); ++i)
	{
		const vxc::BasinEntry& B = (*Basins)[i];
		if (!B.holdsWater())
		{
			continue; // a dry playa has no sheet, by the same rule the sampler uses
		}
		// Inclusive pixel bbox -> world UU, taking the OUTER edge of the last
		// pixel so the rectangle covers the pixels rather than their centres
		// (the same convention BuildLakeSheetRects uses).
		FLakeSheetBasin Info;
		Info.TileX = TileX;
		Info.TileY = TileY;
		Info.BasinId = int32(i);
		Info.MinXUU = VoxelCoords::MmToWorld((OxPx + B.bboxX0) * PixelMm);
		Info.MinYUU = VoxelCoords::MmToWorld((OyPx + B.bboxY0) * PixelMm);
		Info.MaxXUU = VoxelCoords::MmToWorld((OxPx + B.bboxX1 + 1) * PixelMm);
		Info.MaxYUU = VoxelCoords::MmToWorld((OyPx + B.bboxY1 + 1) * PixelMm);
		// THE LEDGER-ADJUSTED DATUM, not the bare wire field (Phase 2). The near
		// field already reads it -- ImplicitFillAtVoxel goes through
		// waterSurfaceMmAtVoxel, which goes through LakeSampler::surfaceAtPixel,
		// which asks the same seam -- so if the sheet kept reading surfaceMm a
		// credited lake would render at two different heights depending on
		// whether you were inside the 52 m disc. That seam was closed once
		// already when the sheet shipped and this is what keeps it closed.
		Info.SurfaceZUU = VoxelCoords::MmToWorld(int64(Impl->Water->basinDatumMm(TileX, TileY, B)));
		if (Info.MaxXUU < CenterXUU - RadiusUU || Info.MinXUU > CenterXUU + RadiusUU ||
		    Info.MaxYUU < CenterYUU - RadiusUU || Info.MinYUU > CenterYUU + RadiusUU)
		{
			continue;
		}
		Out.Add(Info);
	}
	return Out.Num() - Before;
}

int32 UVoxelWaterSubsystem::BuildLakeSheetRects(const FLakeSheetBasin& Basin, int32 StepPx,
                                                 TArray<FBox2D>& OutRectsUU, bool& bOutResolved) const
{
	bOutResolved = false;
	if (!Impl || !Impl->Water)
	{
		return 0;
	}
	check(IsInGameThread());

	const std::vector<vxc::BasinEntry>* Basins = Impl->Water->basinsForTile(Basin.TileX, Basin.TileY);
	if (Basins == nullptr || Basin.BasinId < 0 || size_t(Basin.BasinId) >= Basins->size())
	{
		return 0;
	}
	const vxc::BasinEntry& B = (*Basins)[size_t(Basin.BasinId)];
	const std::vector<uint8_t>* Mask =
		Impl->Water->extentMaskFor(Basin.TileX, Basin.TileY, uint16(Basin.BasinId));
	if (Mask == nullptr)
	{
		// The tile or one of its elevation blocks would not decode. Reporting
		// this apart from "no water" is the whole reason bOutResolved exists:
		// a lake that failed to load looks exactly like a lake that is dry.
		return 0;
	}
	bOutResolved = true;

	const int64 TileSize = int64(Impl->Water->tilePixels());
	const int64 PixelMm = int64(Impl->Water->pixelSizeMm());
	if (TileSize <= 0 || PixelMm <= 0)
	{
		return 0;
	}
	const int64 OxPx = int64(Basin.TileX) * TileSize, OyPx = int64(Basin.TileY) * TileSize;

	std::vector<vxc::LakeSheetRect> Rects;
	vxc::lakeSheetRects(B, *Mask, FMath::Max(StepPx, 1), Rects);

	const int32 Before = OutRectsUU.Num();
	OutRectsUU.Reserve(OutRectsUU.Num() + int32(Rects.size()));
	for (const vxc::LakeSheetRect& R : Rects)
	{
		OutRectsUU.Add(FBox2D(
			FVector2D(VoxelCoords::MmToWorld((OxPx + R.x0) * PixelMm),
			          VoxelCoords::MmToWorld((OyPx + R.y0) * PixelMm)),
			FVector2D(VoxelCoords::MmToWorld((OxPx + R.x1 + 1) * PixelMm),
			          VoxelCoords::MmToWorld((OyPx + R.y1 + 1) * PixelMm))));
	}
	return OutRectsUU.Num() - Before;
}

bool UVoxelWaterSubsystem::GetImplicitWaterDiscUU(FBox2D& OutXY, double& OutMinZUU, double& OutMaxZUU) const
{
	if (!Impl || !Impl->bImplicitCenterValid)
	{
		return false;
	}
	// EXACTLY the sweep in RefreshImplicitWater: bricks [C-R, C+R] inclusive on
	// each axis, each vxc::WaterBrick8::kEdge voxels of VoxelCoords::VoxelSizeUU.
	// Derived from the same two constants rather than restated as metres, so a
	// change to the disc cannot leave the sheet cutting the wrong hole.
	const double BrickUU = double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
	const VoxelCoords::FVoxelCoord C = Impl->LastImplicitCenterBrick;
	OutXY = FBox2D(FVector2D(double(C.X - kImplicitRadiusBricks) * BrickUU,
	                         double(C.Y - kImplicitRadiusBricks) * BrickUU),
	               FVector2D(double(C.X + kImplicitRadiusBricks + 1) * BrickUU,
	                         double(C.Y + kImplicitRadiusBricks + 1) * BrickUU));
	OutMinZUU = double(C.Z - kImplicitRadiusBricksZ) * BrickUU;
	OutMaxZUU = double(C.Z + kImplicitRadiusBricksZ + 1) * BrickUU;
	return true;
}

// --- FAR-FIELD RIVER RIBBONS ------------------------------------------------
//
// The engine half of voxelcore/riverribbon.h. Same split of responsibility the
// lake sheet trio above uses: every geometric decision is in voxel-core, where
// it is unit-tested in ctest without an engine, and this half only addresses
// tiles, budgets the work and converts millimetres to world units.
//
// ONE FINE PIXEL PER BAND ROW IS 256 PIXELS, the water block edge, because
// riverRibbonFillWet iterates BLOCK-MAJOR and decodes each 256x256 block
// exactly once. Handing it a band that is not block-aligned costs nothing (it
// snaps outward internally) but handing it a one-ROW band would decode the
// whole block row per row -- the factor of 256 the producer's own comment
// warns about.
namespace
{
// A water block is 256 fine pixels on a side; a band is one block row.
constexpr int32 kRibbonBandRows = 256;
} // namespace

int32 UVoxelWaterSubsystem::BeginRiverRibbonWindow(double CenterXUU, double CenterYUU, double RadiusUU)
{
	AbandonRiverRibbonWindow();
	if (!Impl || !Impl->Water || RadiusUU <= 0.0)
	{
		return 0;
	}
	check(IsInGameThread());

	// pixelSizeMm() is a CONSTANT on the fine tier (30 m / 16 = 1875 mm), not
	// something read back off a loaded tile, so the window can be laid out
	// before any tile has been touched. tileSize() is not, which is why
	// EnsureTileForPixel addresses from vxc::kFineTileSize instead.
	const int64 PixelMm = int64(Impl->Water->pixelSizeMm());
	if (PixelMm <= 0)
	{
		return 0;
	}
	const int64 CxPx = vxc::floorDiv(VoxelCoords::WorldToMm(CenterXUU), PixelMm);
	const int64 CyPx = vxc::floorDiv(VoxelCoords::WorldToMm(CenterYUU), PixelMm);
	const int64 RadiusPx = FMath::Max<int64>(1, (VoxelCoords::WorldToMm(RadiusUU) + PixelMm - 1) / PixelMm);
	const int64 Edge = 2 * RadiusPx + 1;
	if (Edge > INT32_MAX / 4)
	{
		return 0; // an unreasonable radius; the mask is one byte per pixel
	}

	Impl->RibbonWindow = std::make_unique<vxc::RiverWetWindow>();
	Impl->RibbonWindow->resize(CxPx - RadiusPx, CyPx - RadiusPx, int32_t(Edge), int32_t(Edge));
	Impl->RibbonPixelMm = int32(PixelMm);
	Impl->RibbonBands = int32((Edge + kRibbonBandRows - 1) / kRibbonBandRows);
	return Impl->RibbonBands;
}

bool UVoxelWaterSubsystem::FillRiverRibbonWindowBand(int32 BandIndex, int64& OutWetPixels)
{
	OutWetPixels = 0;
	if (!Impl || !Impl->Water || !Impl->RibbonWindow || BandIndex < 0 || BandIndex >= Impl->RibbonBands)
	{
		return false;
	}
	check(IsInGameThread()); // decodes water blocks: disk I/O plus zstd

	vxc::FineTileSampler* Tiles = Impl->Water->ribbonTiles();
	if (Tiles == nullptr)
	{
		return false; // no fine tier; the same supported "no baked water" world
	}
	vxc::RiverWetWindow& Win = *Impl->RibbonWindow;
	const int32 Y0 = BandIndex * kRibbonBandRows;
	const int32 Rows = FMath::Min(kRibbonBandRows, Win.h - Y0);
	if (Rows <= 0)
	{
		return false;
	}

	// Pull in the tiles this band needs FIRST. riverRibbonFillWet answers DRY
	// for a tile that is not resident -- the same policy surfaceAtPixel
	// applies -- so a band filled before its tile loaded would be a silently
	// missing river rather than an error, which is the failure mode this whole
	// producer exists to make visible.
	//
	// WARMED THROUGH waterSurfaceMmAtVoxel, the ordinary column query, rather
	// than through a bespoke loader: that call already does load-then-ask, so
	// one query per tile puts the tile in the SAME residency state the near
	// field would have put it in, and there is no second code path that could
	// load a different tile set. Addressed from vxc::kFineTileSize because
	// FineTileSampler::tileSize() is read back OFF a loaded tile and is 0 until
	// one is -- using it here would divide by the answer this loop exists to
	// obtain, and no tile would ever load.
	const int64 PixelMm = int64(Impl->RibbonPixelMm);
	const int64 Ax0 = Win.x0, Ax1 = Win.x0 + Win.w - 1;
	const int64 Ay0 = Win.y0 + Y0, Ay1 = Win.y0 + Y0 + Rows - 1;
	const int64 Tile = int64(vxc::kFineTileSize);
	for (int64 Ty = vxc::floorDiv(Ay0, Tile); Ty <= vxc::floorDiv(Ay1, Tile); ++Ty)
	{
		for (int64 Tx = vxc::floorDiv(Ax0, Tile); Tx <= vxc::floorDiv(Ax1, Tile); ++Tx)
		{
			// Any pixel inside the tile will do; take the one nearest the band
			// so a partially covered tile is still addressed inside itself.
			const int64 Px = FMath::Clamp(Tx * Tile, Ax0, Ax1);
			const int64 Py = FMath::Clamp(Ty * Tile, Ay0, Ay1);
			Impl->Water->waterSurfaceMmAtVoxel(vxc::floorDiv(Px * PixelMm, int64(vxc::kVoxelSizeMm)),
			                                   vxc::floorDiv(Py * PixelMm, int64(vxc::kVoxelSizeMm)));
		}
	}

	OutWetPixels = int64(vxc::riverRibbonFillWet(*Tiles, Win, 0, Y0, Win.w, Rows));
	return true;
}

int32 UVoxelWaterSubsystem::FinishRiverRibbonWindow(TArray<FRiverRibbonPathUU>& OutPaths, int64& OutWetPixels,
                                                    int64& OutCentrePixels, int64& OutUnresolvedBlocks)
{
	OutWetPixels = 0;
	OutCentrePixels = 0;
	OutUnresolvedBlocks = 0;
	if (!Impl || !Impl->Water || !Impl->RibbonWindow)
	{
		return 0;
	}
	check(IsInGameThread());

	vxc::RiverSampler* Rivers = Impl->Water->ribbonRivers();
	if (Rivers == nullptr)
	{
		AbandonRiverRibbonWindow();
		return 0;
	}
	vxc::RiverWetWindow& Win = *Impl->RibbonWindow;
	const int32 PixelMm = Impl->RibbonPixelMm;

	vxc::RiverThinField Thin;
	vxc::riverRibbonThin(Win, PixelMm, Thin);
	vxc::riverRibbonResolveDatum(*Rivers, Win, Thin);

	std::vector<vxc::RiverRibbonPath> Paths;
	const vxc::RiverTraceParams Trace;
	const vxc::RiverSimplifyParams Simplify;
	const size_t Traced = vxc::riverRibbonTrace(Win, Thin, PixelMm, Trace, Paths);
	for (size_t i = 0; i < Paths.size(); ++i)
	{
		vxc::riverRibbonSimplify(Paths[i], Simplify);
	}

	OutWetPixels = int64(Thin.wetPixels);
	OutCentrePixels = int64(Thin.centrePixels);
	// Non-zero means a river's bytes WERE there and did not decode. On screen
	// that is indistinguishable from a dry valley, which is why it is reported
	// rather than inferred.
	OutUnresolvedBlocks = int64(Rivers->unresolvedBlocks());

	const int32 Before = OutPaths.Num();
	for (const vxc::RiverRibbonPath& P : Paths)
	{
		FRiverRibbonPathUU Out;
		Out.Points.Reserve(int32(P.pts.size()));
		for (const vxc::RiverRibbonPoint& Pt : P.pts)
		{
			// A traced point is wet by construction, but a datum that failed to
			// resolve is kNoWaterMm == INT32_MIN; letting that through would put
			// a vertex 2,147 km below the world and stretch one ribbon quad
			// across the whole scene. Dropped rather than clamped.
			if (Pt.surfaceMm == vxc::kNoWaterMm)
			{
				continue;
			}
			FRiverRibbonVertexUU V;
			// PIXEL CENTRE, not corner: halfWidthMm is measured about the
			// centre of the run, so anchoring the vertex at the corner would
			// bias the whole ribbon half a pixel (0.94 m) off the channel.
			V.XUU = VoxelCoords::MmToWorld(Pt.px * int64(PixelMm) + int64(PixelMm) / 2);
			V.YUU = VoxelCoords::MmToWorld(Pt.py * int64(PixelMm) + int64(PixelMm) / 2);
			V.ZUU = VoxelCoords::MmToWorld(int64(Pt.surfaceMm));
			V.HalfWidthUU = VoxelCoords::MmToWorld(int64(Pt.halfWidthMm));
			Out.Points.Add(V);
		}
		// A path that lost points to the datum guard can fall below two, which
		// is not a ribbon.
		if (Out.Points.Num() >= 2)
		{
			OutPaths.Add(MoveTemp(Out));
		}
	}
	(void)Traced;

	AbandonRiverRibbonWindow();
	return OutPaths.Num() - Before;
}

// --- THE BASIN VOLUME LEDGER: the public surface (Phase 2) ------------------

int64 UVoxelWaterSubsystem::CreditBasinVolume(int32 TileX, int32 TileY, int32 LocalBasinId, int64 Units,
                                              int32& OutLevelMm, int32& OutBakedMm)
{
	OutLevelMm = 0;
	OutBakedMm = 0;
	if (!Impl || !Impl->Basins || !Impl->Water)
	{
		return 0;
	}
	check(IsInGameThread()); // may load a tile and build a hypsometry; see FineTileBasinTerrain

	// Resolve the ROW first, through the sampler that already streams the tile,
	// so "no such basin" is answered before any ledger state is touched and the
	// baked datum is available to report against.
	const std::vector<vxc::BasinEntry>* Rows = Impl->Water->basinsForTile(TileX, TileY);
	if (Rows == nullptr || LocalBasinId < 0 || size_t(LocalBasinId) >= Rows->size())
	{
		return 0;
	}
	const vxc::BasinEntry& Row = (*Rows)[size_t(LocalBasinId)];
	OutBakedMm = Row.surfaceMm;

	const vxc::BasinId Id = vxc::BasinId::fromTile(TileX, TileY, uint16(LocalBasinId));
	const int64 Accepted = CreditBasinAndSync(*Impl, Id, Units);
	OutLevelMm = Impl->Water->basinDatumMm(TileX, TileY, Row);
	return Accepted;
}

void UVoxelWaterSubsystem::GetBasinLedgerStats(bool& bOutLedgerActive, int32& OutBasins, int64& OutSumUnits,
                                               int64& OutSpilledUnits, int64& OutRoutedUnits,
                                               int64& OutRefundedUnits) const
{
	bOutLedgerActive = Impl && Impl->Basins;
	OutBasins = 0;
	OutSumUnits = 0;
	OutSpilledUnits = 0;
	OutRoutedUnits = 0;
	OutRefundedUnits = 0;
	if (!bOutLedgerActive)
	{
		return;
	}
	OutBasins = int32(Impl->Basins->basinCount());
	OutSumUnits = Impl->Basins->sumOfDeltas();
	OutSpilledUnits = Impl->Basins->totalSpilled();
	OutRoutedUnits = Impl->BasinSpillUnitsRouted;
	OutRefundedUnits = Impl->BasinSpillUnitsRefunded;
}

// --- PHASE 3 LIFECYCLE ACCESSORS (see the header block) ---------------------

int32 UVoxelWaterSubsystem::GatherHeadwaterFaucets(double CenterXUU, double CenterYUU, double RadiusUU,
                                                   TArray<FVoxelHeadwaterFaucet>& Out,
                                                   bool& bOutFromBakedHeads)
{
	bOutFromBakedHeads = false;
	if (!Impl || !Impl->Water || RadiusUU <= 0.0)
	{
		return 0;
	}
	check(IsInGameThread()); // decodes tile blocks (fallback arm) / walks tile tables

	vxc::FineTileSampler* Tiles = Impl->Water->ribbonTiles();
	if (Tiles == nullptr)
	{
		return 0;
	}
	const int64 PixelMm = int64(Tiles->pixelSizeMm());
	const uint32 TileSize = Tiles->tileSize();
	if (PixelMm <= 0 || TileSize == 0)
	{
		return 0;
	}

	const int64 MinPx = vxc::floorDiv(VoxelCoords::WorldToMm(CenterXUU - RadiusUU), PixelMm);
	const int64 MaxPx = vxc::floorDiv(VoxelCoords::WorldToMm(CenterXUU + RadiusUU), PixelMm);
	const int64 MinPy = vxc::floorDiv(VoxelCoords::WorldToMm(CenterYUU - RadiusUU), PixelMm);
	const int64 MaxPy = vxc::floorDiv(VoxelCoords::WorldToMm(CenterYUU + RadiusUU), PixelMm);
	const int32 Before = Out.Num();

	// Source 1: the baked SECTION_HEADWATERS table (bake_ver 24). Exact points
	// with per-head Q. "Any overlapped tile has a RESIDENT table" is the
	// eligibility test -- a tile that merely predates bv24 must not veto the
	// tiles that carry the data.
	const int32 Tx0 = int32(vxc::floorDiv(MinPx, int64(TileSize)));
	const int32 Tx1 = int32(vxc::floorDiv(MaxPx, int64(TileSize)));
	const int32 Ty0 = int32(vxc::floorDiv(MinPy, int64(TileSize)));
	const int32 Ty1 = int32(vxc::floorDiv(MaxPy, int64(TileSize)));
	bool bAnyHeadsTable = false;
	for (int32 Ty = Ty0; Ty <= Ty1; ++Ty)
	{
		for (int32 Tx = Tx0; Tx <= Tx1; ++Tx)
		{
			const vxc::FineTile* T = Tiles->findTile(Tx, Ty);
			if (T == nullptr || !T->hasHeads() || !T->headsResident())
			{
				continue;
			}
			bAnyHeadsTable = true;
			const int64 Ox = int64(Tx) * int64(TileSize);
			const int64 Oy = int64(Ty) * int64(TileSize);
			for (const vxc::HeadEntry& H : T->heads())
			{
				const int64 Px = Ox + int64(H.px);
				const int64 Py = Oy + int64(H.py);
				if (Px < MinPx || Px > MaxPx || Py < MinPy || Py > MaxPy)
				{
					continue;
				}
				FVoxelHeadwaterFaucet F;
				// Pixel CENTRE, the same convention every other consumer of a
				// fine pixel uses (a corner-anchored faucet is half a pixel --
				// 0.94 m -- off its channel).
				F.XUU = VoxelCoords::MmToWorld(Px * PixelMm + PixelMm / 2);
				F.YUU = VoxelCoords::MmToWorld(Py * PixelMm + PixelMm / 2);
				F.QM3PerYear = double(H.qM3PerYear);
				Out.Add(F);
			}
		}
	}
	if (bAnyHeadsTable)
	{
		bOutFromBakedHeads = true;
		return Out.Num() - Before;
	}

	// Source 2 (bv23 fallback): a throwaway baked-water graph over the same
	// box; its no-incoming-segment nodes are the heads. rivernet.h's caveat is
	// inherited knowingly: reaches ENTERING the box read as heads at the rim,
	// and Q is unknowable here (build-time discharge is catchment AREA, not a
	// rate), so QM3PerYear stays 0 and the caller substitutes its default.
	vxc::FineTileBakedWaterSource Source(*Tiles);
	vxc::RiverNetwork Net;
	vxc::BakedWaterBuildParams Params;
	Params.bounds = vxc::RegionBounds{MinPx, MinPy, MaxPx, MaxPy};
	Net.buildFromBakedWater(Source, Impl->Terrain.GetSeed(), Params);
	for (const uint32 NodeId : Net.headwaterNodes())
	{
		const vxc::RiverNode& N = Net.nodes()[size_t(NodeId)];
		const FVector C = VoxelCoords::VoxelToWorldCenter(VoxelCoords::FVoxelCoord{N.vx, N.vy, 0});
		FVoxelHeadwaterFaucet F;
		F.XUU = C.X;
		F.YUU = C.Y;
		F.QM3PerYear = 0.0;
		Out.Add(F);
	}
	return Out.Num() - Before;
}

int64 UVoxelWaterSubsystem::InjectRiverInflowNearVoxel(int64 Vx, int64 Vy, int64 Units, int64 MaxReachMm)
{
	if (!Impl || !Impl->Rivers || Units <= 0 || MaxReachMm <= 0)
	{
		return 0;
	}
	const uint32 Seg = Impl->Rivers->nearestSegmentToVoxel(Vx, Vy, MaxReachMm);
	if (Seg == vxc::RiverNetwork::kNoSegment)
	{
		return 0;
	}
	// injectInflow takes int32; a >2^31-unit hand-off (8.4 GL in one call)
	// would only happen on a pathological backlog, but looping costs nothing
	// and truncating would break the all-or-nothing contract the header gives.
	int64 Remaining = Units;
	while (Remaining > 0)
	{
		const int32 Chunk = int32(vxc::clampi64(Remaining, 0, INT32_MAX));
		Impl->Rivers->injectInflow(Seg, Chunk);
		Remaining -= int64(Chunk);
	}
	return Units;
}

void UVoxelWaterSubsystem::SetFluidSpillIntercept(bool bEnabled, double MinXUU, double MinYUU,
                                                  double MaxXUU, double MaxYUU)
{
	if (!Impl)
	{
		return;
	}
	Impl->bFluidSpillInterceptEnabled = bEnabled;
	Impl->FluidSpillBoxMinXUU = MinXUU;
	Impl->FluidSpillBoxMinYUU = MinYUU;
	Impl->FluidSpillBoxMaxXUU = MaxXUU;
	Impl->FluidSpillBoxMaxYUU = MaxYUU;
	// Disabling refunds everything still held -- RouteBasinSpills' flush does
	// it on the next fixed step, which keeps the refund on the same code path
	// (and the same counters) as the grace-window timeout.
}

int32 UVoxelWaterSubsystem::DrainFluidSpillFaucets(TArray<FVoxelFluidSpillFaucet>& Out)
{
	if (!Impl || Impl->FluidSpillHeld.empty())
	{
		return 0;
	}
	const int32 Before = Out.Num();
	for (const auto& Held : Impl->FluidSpillHeld)
	{
		const FVector OutletUU = VoxelCoords::VoxelToWorldCenter(
			VoxelCoords::FVoxelCoord{Held.Event.outletVx, Held.Event.outletVy, 0});
		FVoxelFluidSpillFaucet F;
		F.BasinKey = Held.Event.basin.v;
		F.Units = Held.Event.units;
		F.OutletXUU = OutletUU.X;
		F.OutletYUU = OutletUU.Y;
		F.SpillZUU = VoxelCoords::MmToWorld(int64(Held.Event.spillMm));
		Out.Add(F);
	}
	Impl->FluidSpillHeld.clear();
	return Out.Num() - Before;
}

int64 UVoxelWaterSubsystem::RefundSpillUnits(uint64 BasinKey, int64 Units)
{
	if (!Impl || !Impl->Basins || Units <= 0)
	{
		return 0;
	}
	const int64 Given = Impl->Basins->refundSpill(vxc::BasinId{BasinKey}, Units);
	if (Given > 0)
	{
		Impl->BasinSpillUnitsRefunded += Given;
		if (Impl->Water)
		{
			Impl->Water->invalidateBasinDatumMemo(); // the lake rose back up
		}
		Impl->BasinDirtySinceLastBroadcast.Add(BasinKey);
	}
	return Given;
}

void UVoxelWaterSubsystem::AbandonRiverRibbonWindow()
{
	if (Impl)
	{
		Impl->RibbonWindow.reset();
		Impl->RibbonBands = 0;
		Impl->RibbonPixelMm = 0;
	}
}

void UVoxelWaterSubsystem::GetMobilizationStats(int32& OutMobilizedBricks, uint64& OutDebited, uint64& OutCredited,
                                                 uint64& OutShortfall) const
{
	OutMobilizedBricks = Impl ? int32(Impl->Mob.mobilizedBricks().size()) : 0;
	OutDebited = Impl ? Impl->Mob.debitedVolume() : 0;
	OutCredited = Impl ? Impl->Mob.creditedVolume() : 0;
	OutShortfall = Impl ? Impl->Mob.shortfallVolume() : 0;
}

bool UVoxelWaterSubsystem::ApplyReplicatedWaterDiffs(const TArray<uint8>& Bytes)
{
	if (!Impl)
	{
		return false;
	}
	vxc::ByteReader R(Bytes.GetData(), size_t(Bytes.Num()));
	uint32_t Count = 0;
	if (!R.u32(Count))
	{
		return Bytes.Num() == 0; // an empty batch is valid (nothing to apply)
	}

	for (uint32_t i = 0; i < Count; ++i)
	{
		int32_t X = 0, Y = 0, Z = 0;
		if (!R.i32(X) || !R.i32(Y) || !R.i32(Z))
		{
			return false;
		}
		const vxc::BrickKey Key{X, Y, Z};

		// HOW A CLIENT AGREES WITH THE SERVER ON WHAT HAS MOBILIZED (C8).
		// Mobilization is authority-only: it depends on where players dug and
		// when, so a client that ran its own front off its replication-mirror
		// CA would drift the instant a packet was late. Instead the authority
		// mobilizes and the client learns it from THIS stream, at zero extra
		// bytes -- mobilizing a brick writes fill into it, which dirties it,
		// which is exactly what puts it in this batch. So "the server sent me
		// authoritative fill for this brick" IS "this brick has mobilized",
		// and marking it here flips the client's own ownership partition in
		// the same packet that delivers the water.
		//
		// markMobilized credits nothing, which is the point: those units are
		// already in the replicated fill below, and crediting them again would
		// duplicate them (waterca.h).
		Impl->Mob.markMobilized(Key);

		for (int z = 0; z < vxc::WaterBrick8::kEdge; ++z)
		{
			for (int y = 0; y < vxc::WaterBrick8::kEdge; ++y)
			{
				for (int x = 0; x < vxc::WaterBrick8::kEdge; ++x)
				{
					uint8_t Fill = 0;
					if (!R.u8(Fill))
					{
						return false;
					}
					const int64_t Vx = int64_t(Key.x) * vxc::WaterBrick8::kEdge + x;
					const int64_t Vy = int64_t(Key.y) * vxc::WaterBrick8::kEdge + y;
					const int64_t Vz = int64_t(Key.z) * vxc::WaterBrick8::kEdge + z;
					Impl->CA.setReplicatedFill(Vx, Vy, Vz, Fill);
				}
			}
		}
		Impl->DirtyBricks.Add(ToCoord(Key));
	}

	// --- THE KEY-REMOVAL SECTION (work item 9c) -----------------------------
	//
	// Read AFTER the fills, because that is the authority's own drain order
	// (takeRecentlyMobilized before takeRecentlyDemoted, waterca.h) and applying
	// them the other way round would leave a brick that mobilized and demoted in
	// one window stuck as "mobilized" on the client forever.
	//
	// ABSENT IS VALID. A batch from a build that predates this section simply
	// ends here, and so does a batch that had no demotions; both leave the
	// client exactly where the pre-9c code left it.
	uint32_t RemovalCount = 0;
	if (R.u32(RemovalCount))
	{
		for (uint32_t i = 0; i < RemovalCount; ++i)
		{
			int32_t X = 0, Y = 0, Z = 0;
			if (!R.i32(X) || !R.i32(Y) || !R.i32(Z))
			{
				return false;
			}
			const vxc::BrickKey Key{X, Y, Z};
			// markDemoted, NOT demoteBrick. The exact predicate is a decision
			// only the authority is entitled to make -- a client's mirror lags by
			// up to a broadcast interval, so it would refuse where the authority
			// accepted and the two would diverge permanently. This applies the
			// authority's conclusion as an instruction (waterca.h "AUTHORITY
			// ONLY"). A key that was never mobilized here is a harmless no-op,
			// which is what makes a duplicated packet safe.
			if (Impl->Mob.markDemoted(Impl->CA, Key))
			{
				Impl->DirtyBricks.Add(ToCoord(Key));
			}
		}
	}

	// --- THE BASIN-LEDGER SECTION (Phase 2) ---------------------------------
	//
	// TAG-GATED, unlike the two positional sections above, because this is the
	// third one and positional would now be ambiguous -- see SerializeWaterDiffs
	// for the argument. A batch from a build that predates this section, or one
	// in which no lake moved, simply has nothing here and the loop never runs.
	//
	// APPLIED WITH restoreDelta, NOT credit(). A client is a MIRROR: the
	// authority already decided how much of a credit the basin kept and how much
	// spilled, and re-running that decision here against a locally reconstructed
	// hypsometry (which may differ if the client has different tiles resident)
	// would let the two drift and would double-count the spill. The authority
	// sends the CONCLUSION -- this basin's delta is now N -- and this applies it,
	// exactly as markMobilized/markDemoted apply the authority's conclusions
	// above rather than re-deriving them.
	uint32_t BasinTag = 0;
	if (R.u32(BasinTag) && BasinTag == kBasinSectionTag)
	{
		uint32_t Version = 0, BasinCount = 0;
		if (!R.u32(Version) || !R.u32(BasinCount))
		{
			return false;
		}
		if (Version != kBasinSectionVersion)
		{
			// A future version's rows are not parseable here, and guessing at
			// them would put lakes at invented levels. Stop reading -- the fills
			// and removals above have already been applied and are still valid.
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("ApplyReplicatedWaterDiffs: basin section is v%u, this build reads v%u. Lake levels will not ")
			       TEXT("track the server."),
			       Version, kBasinSectionVersion);
		}
		else if (Impl->Basins)
		{
			for (uint32_t i = 0; i < BasinCount; ++i)
			{
				uint64_t Key = 0, Raw = 0;
				if (!R.u64(Key) || !R.u64(Raw))
				{
					return false;
				}
				if (Key == 0)
				{
					continue; // kNoBasin is not a basin; a row for it is noise
				}
				Impl->Basins->restoreDelta(vxc::BasinId{Key}, int64_t(Raw));
			}
			// Re-seat the running totals so the mirror's own conserves() stays
			// true. It is a FICTION on a client -- these deltas were credited on
			// the server, not here -- and it is the right fiction: the
			// alternative is a client whose conservation check reads as broken
			// for the whole session, which would make the one instrument that
			// catches a real leak useless exactly where a leak is hardest to
			// see.
			Impl->Basins->restoreTotals();
			// The sampler memoises one column's datum, and every drawn lake
			// height reads through it. Not dropping it here is a lake that
			// visibly lags the server by one query.
			Impl->Water->invalidateBasinDatumMemo();
			Impl->BasinReplicatedRows += int64(BasinCount);
		}
	}

	// Tear down any implicit-water components for bricks this batch converted,
	// so the client's handover looks identical to the authority's. Drains the
	// demotion queue markDemoted just filled as well, so the two feeds cannot
	// accumulate on a client that never runs a fixed step.
	MarkMobilizedBricksDirty(*Impl);
	Impl->Mob.takeRecentlyDemoted();

	// Clients render too (v0: same re-mesh path, driven by replicated state
	// instead of a locally-stepped CA).
	RemeshDirtyBricks(*Impl, ChunkOwner, ChunkRoot, WaterMaterial);
	return true;
}

// --- ADR-0005 water persistence: public save + round-trip verification ------

bool UVoxelWaterSubsystem::SaveWaterState() const
{
	if (!Impl)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("SaveWaterState: no water Impl -- nothing to save."));
		return false;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("SaveWaterState: refused on NM_Client -- only the authority (server/listen/standalone) has an authoritative CA to persist."));
		return false;
	}

	// W4 (ADR-0004): flush the shallow-water sheet back into the CA BEFORE the
	// serializer looks at it. Sheet depth is real volume in the same fill units
	// (swe.h S2, "255 fill units == one full voxel"), and vxc::WaterState::
	// serialize walks the CA's bricks and the mobilizer's key set -- it has
	// never heard of an SweGrid and must not have to. Flushing here is what
	// keeps ADR-0005's blob format, kWaterCAVersion and the whole load path
	// completely untouched by this feature: a save written with voxel.Water.SWE
	// armed is byte-comparable with one written without it, because by the time
	// the bytes are produced the sheet is empty and every unit is back in a CA
	// cell. No-op (and silent) when nothing is armed. Total no-op cost on the
	// untouched path: one null pointer test.
	//
	// A const method mutating simulation state deserves a word: TUniquePtr's
	// constness is shallow, so *Impl is a mutable FVoxelWaterImpl& here, and the
	// flush is genuinely part of "produce a correct save" rather than a side
	// effect of it. The alternative -- serialising and losing the sheet -- is
	// silent data loss, which is not a trade const-correctness wins.
	FlushSweIntoCA(*Impl, TEXT("SaveWaterState"));

	return SaveWaterStateToDisk(*Impl, Impl->Terrain.GetSeed());
}

bool UVoxelWaterSubsystem::VerifyWaterDiskRoundTrip(uint64& OutLiveDigest, uint64& OutReloadedDigest, uint64& OutLiveVolume,
                                                     uint64& OutReloadedVolume, int32& OutLiveMobilized, int32& OutReloadedMobilized)
{
	OutLiveDigest = OutReloadedDigest = 0;
	OutLiveVolume = OutReloadedVolume = 0;
	OutLiveMobilized = OutReloadedMobilized = 0;
	if (!Impl)
	{
		return false;
	}

	// Live side.
	{
		vxc::Digest D;
		Impl->CA.digest(D);
		OutLiveDigest = D.h;
		OutLiveVolume = Impl->CA.totalVolume();
		OutLiveMobilized = int32(Impl->Mob.mobilizedBricks().size());
	}

	// Read back the ACTUAL on-disk blob (proving the file wiring, not just the
	// in-memory serializer) and apply it into a FRESH CA + mobilizer pair built
	// over the same implicit-flood / terrain-solidity callbacks the live pair
	// uses (FVoxelWaterImpl's constructor) -- this is the load path a genuine
	// reload runs, isolated so it never touches live state.
	const FString Path = GetWaterSaveFilePath(Impl->Terrain.GetSeed());
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("VerifyWaterDiskRoundTrip: could not read %s -- run SaveWaterState first."), *Path);
		return false;
	}

	FVoxelWaterImpl& I = *Impl;
	vxc::WaterMobilizer FreshMob(
		// THE LIVE FIELD ITSELF, not a restatement of it. This lambda used to
		// carry its own copy of the implicit expression, and it had already
		// drifted: it knew about caverns and nothing about baked lakes or the
		// ocean, so a world with either reloaded to a different digest and the
		// verifier would have blamed serialization.
		[&I](int64_t vx, int64_t vy, int64_t vz) -> uint8_t { return I.ImplicitFillAtVoxel(vx, vy, vz); },
		[&I](int64_t vx, int64_t vy, int64_t vz) -> vxc::MaterialId
		{ return I.Terrain.IsSolidAtVoxel(vx, vy, vz) ? vxc::MAT_ROCK : vxc::MAT_AIR; });
	vxc::WaterCA FreshCA(FreshMob.makeSolidFn());

	if (!vxc::WaterState::load(Bytes.GetData(), (size_t)Bytes.Num(), FreshCA, FreshMob))
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("VerifyWaterDiskRoundTrip: vxc::WaterState::load REFUSED the on-disk blob %s."), *Path);
		return false;
	}

	vxc::Digest RD;
	FreshCA.digest(RD);
	OutReloadedDigest = RD.h;
	OutReloadedVolume = FreshCA.totalVolume();
	OutReloadedMobilized = int32(FreshMob.mobilizedBricks().size());
	return true;
}

// --- voxel.SpawnWater console command (task item 2, dev tool) --------------

namespace
{
// ADR-0005 dev/verification convenience: force a water-state save without
// waiting for shutdown. Mirrors the voxel.SaveWorld console command
// (VoxelWorldSubsystem.cpp) -- same manual-save-alongside-autosave shape.
FAutoConsoleCommandWithWorld CVarVoxelSaveWater(
	TEXT("voxel.SaveWater"),
	TEXT("ADR-0005: write the water-state blob (Saved/VoxelWorlds/<seed>.vxwater) now, beside the terrain edit log."),
	FConsoleCommandWithWorldDelegate::CreateLambda(
		[](UWorld* World)
		{
			if (!World)
			{
				return;
			}
			if (UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>())
			{
				Water->SaveWaterState();
			}
			else
			{
				UE_LOG(LogVoxelWater, Warning, TEXT("voxel.SaveWater: no UVoxelWaterSubsystem in this world."));
			}
		}));

// Dumps water at the local player's crosshair. Shared by voxel.SpawnWater and
// voxel.Water.SpawnIn.
void SpawnWaterAtCrosshair(UWorld* World, uint32 Amount, const TCHAR* Caller)
{
	UVoxelWaterSubsystem* WaterSubsystem = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	UVoxelWorldSubsystem* Terrain = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!WaterSubsystem || !Terrain || !PC || !PC->PlayerCameraManager)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("%s: missing subsystem/camera; nothing spawned."), Caller);
		return;
	}

	const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
	const FVector CamDir = PC->PlayerCameraManager->GetCameraRotation().Vector();

	FVector HitVoxelCenterUU, PrevVoxelCenterUU;
	constexpr double MaxDistUU = 6400.0; // 64m, generous crosshair reach
	FVector TargetUU;
	if (Terrain->RaycastVoxelWorld(CamLoc, CamDir, MaxDistUU, HitVoxelCenterUU, PrevVoxelCenterUU))
	{
		TargetUU = PrevVoxelCenterUU; // last empty voxel before the hit surface -- mirrors TryPlace's aim
	}
	else
	{
		TargetUU = CamLoc + CamDir * 500.0; // no hit: 5m in front of the camera
	}

	const uint32 Placed = WaterSubsystem->SpawnWaterAt(TargetUU, Amount);
	UE_LOG(LogVoxelWater, Log, TEXT("%s: requested %u, placed %u at (%.0f,%.0f,%.0f)"), Caller, Amount, Placed,
	       TargetUU.X, TargetUU.Y, TargetUU.Z);
}

// The one thing that makes Phase 2 VISIBLE without a single particle.
//
// The plan calls Phase 2 "shippable on its own -- lakes visibly rise/spill/drain
// with zero particles", and until Phase 3 there is no gameplay source of inflow
// to a basin, so without this the whole authority layer is correct, tested, and
// impossible for anyone to look at. This is the crank you turn to look at it.
//
// Units are WaterCA fill units (255 per 10 cm voxel, so 255,000,000 is 1000 m^3
// of water). A basin is named the way the v1 wire names it -- its fine tile and
// its row index -- which `voxel.Water.Sheets`' own logging already prints, and
// which GatherLakeSheetBasinsInTile hands out as FLakeSheetBasin::BasinId.
FAutoConsoleCommandWithWorldAndArgs CVarVoxelWaterCreditBasin(
	TEXT("voxel.Water.CreditBasin"),
	TEXT("voxel.Water.CreditBasin <tileX> <tileY> <basinId> <units> -- dev tool: adds <units> (WaterCA fill units, ")
	TEXT("255 per 10 cm voxel) to one basin's volume ledger. The lake rises to the level its hypsometry says that ")
	TEXT("volume stands at, and anything past its baked sill spills to the outlet. Negative units debit."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (!World)
			{
				return;
			}
			if (Args.Num() < 4)
			{
				UE_LOG(LogVoxelWater, Warning,
				       TEXT("voxel.Water.CreditBasin <tileX> <tileY> <basinId> <units> -- needs four arguments."));
				return;
			}
			UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>();
			if (!Water)
			{
				return;
			}
			const int32 Tx = FCString::Atoi(*Args[0]);
			const int32 Ty = FCString::Atoi(*Args[1]);
			const int32 Id = FCString::Atoi(*Args[2]);
			const int64 Units = FCString::Atoi64(*Args[3]);

			int32 LevelMm = 0, BakedMm = 0;
			const int64 Accepted = Water->CreditBasinVolume(Tx, Ty, Id, Units, LevelMm, BakedMm);

			bool bActive = false;
			int32 Basins = 0;
			int64 Sum = 0, Spilled = 0, Routed = 0, Refunded = 0;
			Water->GetBasinLedgerStats(bActive, Basins, Sum, Spilled, Routed, Refunded);
			if (!bActive)
			{
				UE_LOG(LogVoxelWater, Warning,
				       TEXT("voxel.Water.CreditBasin: no basin ledger in this world -- it needs the fine tier ")
				       TEXT("(-VoxelFineTileDir), because the basin table lives in the .vxtl."));
				return;
			}
			if (Accepted == 0 && Units > 0)
			{
				// REFUSED, and the two reasons look identical on screen, so say
				// both: the tile may not be streamed in yet, or there may be no
				// such row.
				UE_LOG(LogVoxelWater, Warning,
				       TEXT("voxel.Water.CreditBasin: basin (%d,%d)#%d took NOTHING. Either fine tile (%d,%d) is not ")
				       TEXT("loaded yet (fly over it first), or it has no row %d, or its elevation blocks would not ")
				       TEXT("decode. Nothing was invented."),
				       Tx, Ty, Id, Tx, Ty, Id);
				return;
			}
			UE_LOG(LogVoxelWater, Log,
			       TEXT("voxel.Water.CreditBasin: basin (%d,%d)#%d accepted %lld units -- surface %d mm -> %d mm ")
			       TEXT("(baked equilibrium %d mm). Ledger now: %d basin(s) off equilibrium, sum %lld units, ")
			       TEXT("%lld spilled (%lld routed to the graph, %lld refunded)."),
			       Tx, Ty, Id, (long long)Accepted, BakedMm, LevelMm, BakedMm, Basins, (long long)Sum,
			       (long long)Spilled, (long long)Routed, (long long)Refunded);
		}));

FAutoConsoleCommandWithWorldAndArgs CVarVoxelSpawnWater(
	TEXT("voxel.SpawnWater"),
	TEXT("voxel.SpawnWater <amount> -- dev tool: dumps <amount> water fill units at the local player's crosshair (W2)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (!World)
			{
				return;
			}
			if (Args.Num() < 1)
			{
				UE_LOG(LogVoxelWater, Warning, TEXT("voxel.SpawnWater <amount> -- missing amount argument."));
				return;
			}
			SpawnWaterAtCrosshair(World, uint32(FMath::Max(0, FCString::Atoi(*Args[0]))), TEXT("voxel.SpawnWater"));
		}));

// The headless-verification form of voxel.SpawnWater.
//
// A water A/B screenshot needs water actually in frame, and at a surface anchor
// there may be none: UWaterChunkComponent geometry comes only from the CA
// (player-poured or breached) and from implicit CAVERN lakes, which are
// underground. AVoxelOceanActor's plane is a different primitive with a
// different material and is unaffected by voxel.Water.GPU either way -- so
// screenshotting an anchor that merely has the OCEAN in it would compare two
// images in which no water-brick geometry appears at all, and pass regardless
// of whether the pool works.
//
// -ExecCmds fires at startup, when there is no pawn, no camera and no streamed
// terrain to raycast against, so the spawn has to be deferred. Pairs with
// voxel.Debug.ShotIn: "voxel.Water.GPU 1, voxel.Water.SpawnIn 20 400000,
// voxel.Debug.ShotIn 30" pours at 20 s and shoots at 30 s, by which time the
// cascade has filled and the CA has settled the pour.
FAutoConsoleCommandWithWorldAndArgs CVarVoxelWaterSpawnIn(
	TEXT("voxel.Water.SpawnIn"),
	TEXT("voxel.Water.SpawnIn <seconds> <amount> -- run voxel.SpawnWater <amount> N seconds from now. Headless ")
	TEXT("verification aid: -ExecCmds runs before any pawn or streamed terrain exists, so an immediate pour has ")
	TEXT("nothing to aim at."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (!World)
			{
				return;
			}
			const float Delay = (Args.Num() > 0) ? FMath::Max(0.1f, FCString::Atof(*Args[0])) : 20.0f;
			const uint32 Amount = (Args.Num() > 1) ? uint32(FMath::Max(0, FCString::Atoi(*Args[1]))) : 400000u;

			TWeakObjectPtr<UWorld> WeakWorld(World);
			FTimerHandle Handle;
			World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakWorld, Amount]()
			{
				UWorld* W = WeakWorld.Get();
				UVoxelWaterSubsystem* WaterSubsystem = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
				UVoxelWorldSubsystem* Terrain = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
				APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
				if (!WaterSubsystem || !Terrain || !PC || !PC->PlayerCameraManager)
				{
					UE_LOG(LogVoxelWater, Warning, TEXT("voxel.Water.SpawnIn: missing subsystem/camera; nothing spawned."));
					return;
				}

				// Aim at the GROUND, not at the crosshair.
				//
				// voxel.SpawnWater's crosshair raycast is the right tool in
				// play and the wrong one here. The default spawn pose is +5m
				// above the surface looking horizontally (VoxelEarthGameMode's
				// RestartPlayer), and at a summit anchor like
				// -VoxelSpawnAt=-84480,53760 that aims at the horizon: the
				// 64 m raycast hits nothing, the pour lands in mid-air, and
				// the water falls out of the bottom of the frame while the
				// screenshot photographs empty sky. Sampling the surface
				// height a fixed distance ahead puts the pour on solid ground
				// every time regardless of terrain, and pointing the camera
				// at it makes the A/B frame the water rather than the horizon.
				const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
				const FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();
				FVector Ahead = CamRot.Vector();
				Ahead.Z = 0.0;
				Ahead = Ahead.GetSafeNormal();
				if (Ahead.IsNearlyZero())
				{
					Ahead = FVector(1, 0, 0);
				}

				constexpr double AheadUU = 1200.0;   // 12 m: clear of the pawn, well inside the near ring
				constexpr double AboveUU = 300.0;    // pour from 3 m up so it lands rather than clipping into rock
				const FVector GroundXY = CamLoc + Ahead * AheadUU;
				const double SurfaceZUU = Terrain->GetSurfaceHeightUU(GroundXY.X, GroundXY.Y);
				const FVector TargetUU(GroundXY.X, GroundXY.Y, SurfaceZUU + AboveUU);

				const uint32 Placed = WaterSubsystem->SpawnWaterAt(TargetUU, Amount);
				PC->SetControlRotation((TargetUU - CamLoc).Rotation());
				UE_LOG(LogVoxelWater, Log,
				       TEXT("voxel.Water.SpawnIn: requested %u, placed %u at (%.0f,%.0f,%.0f); camera now looks at it"),
				       Amount, Placed, TargetUU.X, TargetUU.Y, TargetUU.Z);
			}), Delay, false);
			UE_LOG(LogVoxelWater, Log, TEXT("voxel.Water.SpawnIn: %u fill units scheduled in %.1f s"), Amount, Delay);
		}));
} // namespace
