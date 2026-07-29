#include "VoxelEarth.h"

#include "VoxelTileCodec.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (do not include it from headers pulled into UHT-parsed code).
#include "voxelcore/hash.h"
#include "voxelcore/core.h"
#include "voxelcore/amplifier.h"
#include "voxelcore/tiles.h"

DEFINE_LOG_CATEGORY(LogVoxelEarth);

void FVoxelEarthModule::StartupModule()
{
	// Pinned golden from voxel-core/tests/test_hash.cpp — proves this module's
	// headers and the linked voxelcore.lib agree with the determinism goldens
	// (docs/determinism.md).
	constexpr uint64_t Golden = 0x2A4F111B3BE57715ull;
	const uint64_t Computed = vxc::hash2(1, 0, 0, 0);

	UE_LOG(LogVoxelEarth, Log, TEXT("vxc::hash2(1, 0, 0, 0) = 0x%016llX"), Computed);
	UE_LOG(LogVoxelEarth, Log, TEXT("Golden (test_hash.cpp) = 0x%016llX"), Golden);

	if (Computed == Golden)
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("voxel-core determinism golden MATCHES."));
	}
	else
	{
		UE_LOG(LogVoxelEarth, Error, TEXT("voxel-core determinism golden MISMATCH!"));
	}

	UE_LOG(LogVoxelEarth, Log, TEXT("vxc::kWorldGenVersion = %u"), vxc::kWorldGenVersion);

	// Amplifier::column is compiled code in voxelcore.lib (not header-inline),
	// so this call proves the static library itself links and runs — the hash
	// check above only exercises header constexpr.
	vxc::SyntheticTileSampler Tiles(20260719);
	const vxc::Amplifier Amp(20260719, Tiles);
	const vxc::ColumnSample Col = Amp.column(0, 0);
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("voxelcore.lib Amplifier::column(0,0): surface %d mm, surfaceMat %u"),
	       Col.surfaceMm, static_cast<uint32>(Col.surfaceMat));

	// Which zstd (if any) this binary will decode `.vxtl` v2 CODEC_ZSTD tiles
	// with. Stated once, here, because the alternative to reading it in the log
	// is discovering it from a fine tile that will not load -- or worse, from
	// terrain that is quietly wrong. See VoxelTileCodec.h.
	VoxelEarth::LogFineTileCodecStatus();
}

void FVoxelEarthModule::ShutdownModule()
{
}

IMPLEMENT_PRIMARY_GAME_MODULE(FVoxelEarthModule, VoxelEarth, "VoxelEarth");
