#include "VoxelClimateProbe.h"

#include "VoxelEarth.h" // LogVoxelEarth
#include "VoxelWorldSubsystem.h" // DefaultSeed -- shared, not re-typed

#include "HAL/CriticalSection.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h" // DefaultTileDir ini fallback, mirroring the subsystem
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "voxelcore/tiles.h"
#include "voxelcore/tilestore.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace
{
	// Init-once state. Guarded rather than relying on EnsureInitialized() always
	// being reached from the game thread first: UVoxelChunkComponent builds its
	// vertex data on background UE::Tasks, and a chunk job can land before the
	// clipmap actor has ticked. After bInitialized the sampler is never mutated,
	// so the hot path takes no lock at all.
	FCriticalSection GInitLock;
	bool GInitialized = false;
	std::unique_ptr<vxc::ITileSampler> GSampler;
	int32 GLoadedTiles = 0;
	bool GUsingTileGrid = false;

	// -VoxelClimateSentinelGuard=0 turns the floor off. Resolved ONCE, under
	// GInitLock, for the same reason the sampler is: SampleClimateAtWorldUU runs
	// on meshing workers, and FCommandLine::Get() per vertex would be both a
	// waste and a data race waiting to happen. See kSentinelSafeFloorU8 in the
	// header for what the floor is and why it exists.
	bool GSentinelGuard = true;

	// Deliberately a copy of VoxelWorldSubsystem.cpp's MakeTileSampler scan,
	// not a call into it: that file is off-limits to this change, and its
	// version is a file-local static anyway. The two must agree about which
	// tiles exist -- if they ever disagree, climate would be sampled from a
	// different tile set than the geometry, so this mirrors it exactly
	// (non-recursive *.vxtl scan, reject on seed/scale mismatch, fall back to
	// synthetic on zero loaded).
	void LoadTiles()
	{
		// PRECEDENCE MUST MATCH UVoxelWorldSubsystem::Initialize EXACTLY:
		// command line wins, then DefaultTileDir from DefaultGame.ini, then
		// nothing. Relative paths resolve against Content/.
		//
		// This block is why the comment above matters. When the ini fallback was
		// added to the subsystem (2026-07-24) it was NOT added here, so on every
		// ordinary launch -- the exact case the ini default exists to fix --
		// geometry loaded the real diffusion tiles while this probe loaded ZERO
		// and fell back to synthetic, handing the renderers a neutral 128/128
		// climate everywhere. The world rendered flat and untextured, and the
		// startup log said so plainly ("SYNTHETIC (no -VoxelTileDir), tiles=0")
		// for anyone who read it. That is precisely the "climate sampled from a
		// different tile set than the geometry" failure the comment above warns
		// about, so keep the two in step.
		FString TileDir;
		if (!FParse::Value(FCommandLine::Get(), TEXT("VoxelTileDir="), TileDir) && GConfig)
		{
			GConfig->GetString(TEXT("/Script/VoxelEarth.VoxelWorldSubsystem"),
			                   TEXT("DefaultTileDir"), TileDir, GGameIni);
		}
		if (!TileDir.IsEmpty() && FPaths::IsRelative(TileDir))
		{
			TileDir = FPaths::Combine(FPaths::ProjectContentDir(), TileDir);
			FPaths::CollapseRelativeDirectories(TileDir);
		}

		int32 TileScale = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelTileScale="), TileScale);

		// Default MUST be UVoxelWorldSubsystem::DefaultSeed, not 0 -- taken from
		// that class rather than re-typed, so this one cannot drift again.
		//
		// The second half of the same divergence as the TileDir block above: with
		// a default of 0, every tile in a directory generated for seed 20260719
		// failed loadTile's seed check, so the probe loaded ZERO tiles and fell
		// back to synthetic even once it was looking at the right directory. The
		// symptom is identical to having no tiles at all -- flat, untextured
		// terrain -- and the log line distinguishes them ("zero tiles loaded
		// from <path>" vs "no -VoxelTileDir").
		uint64 Seed = UVoxelWorldSubsystem::DefaultSeed;
		{
			FString SeedStr;
			if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSeed="), SeedStr))
			{
				Seed = FCString::Strtoui64(*SeedStr, nullptr, 10);
			}
		}

		if (TileDir.IsEmpty() || vxc::tilePixelSizeMm((uint8)TileScale) == 0)
		{
			GSampler = std::make_unique<vxc::SyntheticTileSampler>(Seed);
			return;
		}

		const std::filesystem::path DirPath(*TileDir);
		std::error_code DirEc;
		if (!std::filesystem::is_directory(DirPath, DirEc) || DirEc)
		{
			GSampler = std::make_unique<vxc::SyntheticTileSampler>(Seed);
			return;
		}

		auto Grid = std::make_unique<vxc::TileGridSampler>(Seed, (uint8)TileScale);
		int32 Loaded = 0;

		std::error_code IterEc;
		for (auto It = std::filesystem::directory_iterator(DirPath, IterEc);
		     !IterEc && It != std::filesystem::directory_iterator(); It.increment(IterEc))
		{
			const std::filesystem::directory_entry& Entry = *It;
			std::error_code FileEc;
			if (!Entry.is_regular_file(FileEc) || FileEc) { continue; }
			if (Entry.path().extension() != ".vxtl") { continue; }

			std::optional<std::vector<uint8_t>> Bytes = vxc::readFileBytes(Entry.path());
			if (!Bytes) { continue; }
			std::optional<vxc::TileData> Parsed = vxc::TileData::parse(Bytes->data(), Bytes->size());
			if (!Parsed) { continue; }
			if (!Grid->loadTile(std::move(*Parsed))) { continue; }
			++Loaded;
		}

		if (Loaded == 0)
		{
			UE_LOG(LogVoxelEarth, Error,
			       TEXT("VoxelClimateProbe: zero tiles loaded from %s -- terrain will render with NEUTRAL climate ")
			       TEXT("(mid-LUT) everywhere. Check -VoxelSeed/-VoxelTileScale match the tiles."), *TileDir);
			GSampler = std::make_unique<vxc::SyntheticTileSampler>(Seed);
			return;
		}

		GLoadedTiles = Loaded;
		GUsingTileGrid = true;
		GSampler = std::move(Grid);
	}
}

namespace VoxelClimate
{

void EnsureInitialized()
{
	FScopeLock Lock(&GInitLock);
	if (GInitialized) { return; }
	LoadTiles();

	int32 GuardFlag = 1;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelClimateSentinelGuard="), GuardFlag))
	{
		GSentinelGuard = (GuardFlag != 0);
	}

	GInitialized = true;
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelClimateProbe: %s, tiles=%d, remap temp u8 [%d,%d] precip u8 [%d,%d], sentinel guard %s (floor %d)"),
	       GUsingTileGrid ? TEXT("tile grid") : TEXT("SYNTHETIC (no -VoxelTileDir)"),
	       GLoadedTiles, kTempU8Lo, kTempU8Hi, kPrecipU8Lo, kPrecipU8Hi,
	       GSentinelGuard ? TEXT("ON") : TEXT("OFF -- cold+dry terrain will draw MAGENTA"),
	       GSentinelGuard ? kSentinelSafeFloorU8 : 0);
}

uint8 BiomeTintForFace(uint8 /*MaterialId*/, int32 FaceAxis, bool bFacePositive)
{
	// See the header for why this is geometric: voxel-core emits only MAT_ROCK
	// and MAT_SUBSOIL on this data (measured, -VoxelMatHistogram), so no
	// id-keyed rule can separate a hillside from a cave wall.
	if (FaceAxis == 2)
	{
		return bFacePositive ? 255 : 0; // sky-facing : ceiling
	}
	// Side face. 140/255 = 0.55: the riser of a 10 cm step reads as part turf,
	// part exposed soil. A hard 0 stripes every hillside at voxel pitch, which
	// looks far worse than either extreme.
	return 140;
}

int32 GetLoadedTileCount()
{
	FScopeLock Lock(&GInitLock);
	return GLoadedTiles;
}

FVoxelClimateBytes SampleClimateAtWorldUU(double WorldXUU, double WorldYUU)
{
	if (!GInitialized)
	{
		// A worker beat the game thread to it. EnsureInitialized is idempotent
		// and this is the only path that can contend, so paying the lock here
		// (once, at world start) is cheaper than checking an atomic per vertex.
		EnsureInitialized();
	}

	FVoxelClimateBytes Out;
	vxc::ITileSampler* Sampler = GSampler.get();
	if (Sampler == nullptr)
	{
		return Out; // neutral 128/128
	}

	// Same world -> tile-pixel convention as
	// UVoxelWorldSubsystem::SampleTerrainHeightUU (1 UU = 10 mm). Sharing the
	// convention is what makes climate line up with the elevation the geometry
	// was built from.
	const double PixelSizeUU = double(Sampler->pixelSizeMm()) / 10.0;
	const double Px = WorldXUU / PixelSizeUU;
	const double Py = WorldYUU / PixelSizeUU;
	const int64 Px0 = (int64)FMath::FloorToDouble(Px);
	const int64 Py0 = (int64)FMath::FloorToDouble(Py);
	const double Fx = Px - double(Px0);
	const double Fy = Py - double(Py0);

	// Bilinear across the 30 m raster. Without this the biome colour would step
	// in visible 30 m squares, which at 10 cm voxels is a 300-voxel-wide block.
	const vxc::ClimateSample C00 = Sampler->climate(Px0, Py0);
	const vxc::ClimateSample C10 = Sampler->climate(Px0 + 1, Py0);
	const vxc::ClimateSample C01 = Sampler->climate(Px0, Py0 + 1);
	const vxc::ClimateSample C11 = Sampler->climate(Px0 + 1, Py0 + 1);

	auto Bilerp = [Fx, Fy](double V00, double V10, double V01, double V11)
	{
		return FMath::Lerp(FMath::Lerp(V00, V10, Fx), FMath::Lerp(V01, V11, Fx), Fy);
	};

	const double TempRaw = Bilerp(C00.temperature, C10.temperature, C01.temperature, C11.temperature);
	const double PrecipRaw = Bilerp(C00.precipitation, C10.precipitation, C01.precipitation, C11.precipitation);

	// Remap the measured p1..p99 window onto the full byte. Values outside it
	// clamp rather than wrap -- the window is p1..p99 precisely so the 2% of
	// outliers saturate at the ends of the LUT instead of compressing the 98%.
	//
	// THE FLOOR IS NOT PART OF THE REMAP, it is a guard on what this function is
	// allowed to emit. kSentinelSafeFloorU8's comment has the whole story: the
	// bottom of both channels is reserved by the terrain materials for a debug
	// marker, and terrain that lands there is painted magenta. Applied AFTER the
	// clamp, so it lifts exactly the values the window pushed to zero and leaves
	// every in-window value alone.
	//
	// "p1..p99 means only 1% clamps" is true of the world this window was
	// measured on and false of the one configured today: precipitation clamps to
	// zero on 72.9% of its land. That is a separate defect -- the window itself
	// is stale -- and it CANNOT be fixed here, because kTempU8Lo/Hi and
	// kPrecipU8Lo/Hi are also baked into T_VoxelBiomeLUT's axes by
	// Tools/gen_terrain_textures.py, and moving one without regenerating the
	// other silently shifts every biome. The static_asserts on those constants
	// say so, and they are why this change guards the emission instead of
	// retuning the window.
	const bool bGuard = GSentinelGuard;
	auto Remap = [bGuard](double Raw, int32 Lo, int32 Hi) -> uint8
	{
		const double T = (Raw - double(Lo)) / double(FMath::Max(Hi - Lo, 1));
		const int32 Byte = FMath::Clamp(FMath::RoundToInt(T * 255.0), 0, 255);
		return (uint8)(bGuard ? FMath::Max(Byte, kSentinelSafeFloorU8) : Byte);
	};

	Out.Temperature = Remap(TempRaw, kTempU8Lo, kTempU8Hi);
	Out.Precipitation = Remap(PrecipRaw, kPrecipU8Lo, kPrecipU8Hi);
	return Out;
}

} // namespace VoxelClimate
