// voxel.GPU.VerifyRegion — the G2a gate (ADR-0006).
//
// WHAT THIS PROVES, AND WHY IT IS WORTH A CONSOLE COMMAND.
//
// voxel-core/bench/vxc_gpu.exe already proves the GPU kernels are bit-exact
// with the CPU mesher. But it proves it about kernels compiled by standalone
// DXC and run on raw Vulkan. Unreal compiles the same source with its own
// shader pipeline, binds resources its own way, and runs them on D3D12. None
// of that is guaranteed to produce identical bytes, and "the terrain looks a
// bit wrong" is a terrible way to find out.
//
// So this command runs the SAME two fixture regions the bench runs, in the
// same order, accumulating the digest with the same rules — and prints a
// number that should be *identical* to the one the bench prints. If it is,
// Unreal's toolchain reproduces the proven path exactly. If it is not, the
// per-field mismatch dump says precisely where the two disagree.
//
// It is deliberately headless: no PIE, no flying, no visual judgement.
//
// Usage:
//   voxel.GPU.VerifyRegion            both bench fixture regions (digest gate)
//   voxel.GPU.VerifyRegion 0          origin region only
//   voxel.GPU.VerifyRegion 1          far-negative region only

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "VoxelGpuWorldGen.h"

#include "voxelcore/core.h"
#include "voxelcore/tiles.h"
#include "voxelcore/amplifier.h"
#include "voxelcore/mesher.h"
#include "voxelcore/caverns.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGpuVerify, Log, All);

namespace
{
	// The bench's fixtures, verbatim (gpu_harness.cpp main()). Same seed, same
	// origins, same 64x64 footprint — this is what makes the digests
	// comparable across the two toolchains.
	constexpr uint64 kSeed = 20260719;

	struct FRegionSpec
	{
		const TCHAR* Name;
		int32 OriginVx;
		int32 OriginVy;
		uint32 Width;
		uint32 Height;
	};

	const FRegionSpec kRegions[] = {
		{ TEXT("origin"),        -64,     -64, 64, 64 },
		{ TEXT("far-negative"), -100000, 250000, 64, 64 },
	};

	// How far past the dispatch footprint the raster window must reach.
	//
	// This is NOT slack. VoxelizeMain's cavern pass evaluates the terrain
	// surface at a cave site's own xy, and a site can sit up to
	// kCavernMaxReachMm away from the column that queried it; the shader then
	// floors that to a voxel (one more voxel of reach) and takes bilinear taps
	// at px and px+1. Undersizing the window does not fault — the kernel
	// clamps to the window edge — it just silently produces different terrain
	// from the CPU, which is exactly the failure this command exists to catch.
	constexpr int64 kRasterCavernMarginMm = vxc::kCavernMaxReachMm + vxc::kVoxelSizeMm;

	// Builds one region's GPU request: the raster window, its contents, and the
	// z-range, exactly as gpu_harness.cpp::runRegion does.
	//
	// OutCpuColumns is filled as a side effect because deriving the z-range
	// requires every column anyway, and the comparison needs them again later.
	FVoxelGpuRegionRequest BuildRequest(const FRegionSpec& Region,
	                                    const vxc::Amplifier& CpuAmp,
	                                    vxc::SyntheticTileSampler& Tiles,
	                                    TArray<vxc::ColumnSample>& OutCpuColumns)
	{
		FVoxelGpuRegionRequest Req;
		Req.DispatchColumns = FUintVector2(Region.Width, Region.Height);
		Req.OriginVx = Region.OriginVx;
		Req.OriginVy = Region.OriginVy;
		Req.Seed = kSeed;

		const int64 PixelSizeMm = Tiles.pixelSizeMm();
		Req.PixelSizeMm = static_cast<int32>(PixelSizeMm);

		// --- raster window extent -------------------------------------------
		const int64 XMmMin = int64(Region.OriginVx) * vxc::kVoxelSizeMm - kRasterCavernMarginMm;
		const int64 XMmMax = int64(Region.OriginVx + int32(Region.Width) - 1) * vxc::kVoxelSizeMm
		                   + kRasterCavernMarginMm;
		const int64 YMmMin = int64(Region.OriginVy) * vxc::kVoxelSizeMm - kRasterCavernMarginMm;
		const int64 YMmMax = int64(Region.OriginVy + int32(Region.Height) - 1) * vxc::kVoxelSizeMm
		                   + kRasterCavernMarginMm;

		const int64 PxMin = vxc::floorDiv(XMmMin, PixelSizeMm);
		const int64 PxMax = vxc::floorDiv(XMmMax, PixelSizeMm) + 1;  // +1: second bilinear tap
		const int64 PyMin = vxc::floorDiv(YMmMin, PixelSizeMm);
		const int64 PyMax = vxc::floorDiv(YMmMax, PixelSizeMm) + 1;

		const uint32 RasterW = static_cast<uint32>(PxMax - PxMin + 1);
		const uint32 RasterH = static_cast<uint32>(PyMax - PyMin + 1);

		Req.RasterOriginPx = FIntPoint(static_cast<int32>(PxMin), static_cast<int32>(PyMin));
		Req.RasterSize = FUintVector2(RasterW, RasterH);

		// --- raster contents (row-major, x fastest) -------------------------
		Req.ElevationMm.SetNumUninitialized(RasterW * RasterH);
		Req.ClimatePacked.SetNumUninitialized(RasterW * RasterH);
		for (uint32 Ly = 0; Ly < RasterH; ++Ly)
		{
			for (uint32 Lx = 0; Lx < RasterW; ++Lx)
			{
				const int64 Px = PxMin + Lx;
				const int64 Py = PyMin + Ly;
				const int32 Idx = int32(Lx + Ly * RasterW);
				Req.ElevationMm[Idx] = Tiles.elevationMm(Px, Py);

				const vxc::ClimateSample Cl = Tiles.climate(Px, Py);
				Req.ClimatePacked[Idx] = uint32(Cl.temperature)
				                       | (uint32(Cl.seasonality) << 8)
				                       | (uint32(Cl.precipitation) << 16)
				                       | (uint32(Cl.precipVariability) << 24);
			}
		}

		// --- columns + vertical extent --------------------------------------
		// Mirrors GeneratedWorld<8>::surfaceBrickRange, but reduced over the
		// whole region rather than one 8x8 footprint, because BrickZMin and
		// BricksZ are single scalars shared by the entire dispatch.
		OutCpuColumns.SetNumUninitialized(Region.Width * Region.Height);
		int64 VzMin = MAX_int64;
		int64 VzMax = MIN_int64;
		for (uint32 Y = 0; Y < Region.Height; ++Y)
		{
			for (uint32 X = 0; X < Region.Width; ++X)
			{
				const int64 Vx = int64(Region.OriginVx) + X;
				const int64 Vy = int64(Region.OriginVy) + Y;
				const vxc::ColumnSample C = CpuAmp.column(Vx, Vy);
				OutCpuColumns[int32(X + Y * Region.Width)] = C;

				// Topmost solid voxel: its centre (vz*100 + 50) <= surfaceMm.
				const int64 Top = vxc::floorDiv(int64(C.surfaceMm) - vxc::kVoxelSizeMm / 2,
				                                vxc::kVoxelSizeMm);
				VzMin = FMath::Min(VzMin, Top);
				VzMax = FMath::Max(VzMax, Top);
			}
		}

		const int32 BrickZMin = static_cast<int32>(vxc::floorDiv(VzMin, 8));
		const int32 BrickZMax = static_cast<int32>(vxc::floorDiv(VzMax, 8));
		Req.BrickZMin = BrickZMin;
		Req.BricksZ = static_cast<uint32>(BrickZMax - BrickZMin + 1);

		return Req;
	}

	constexpr uint32 CellIndexInBrick(uint32 X, uint32 Y, uint32 Z)
	{
		return X + 8u * (Y + 8u * Z);
	}

	struct FMismatch
	{
		FString Where;
		int64 Cpu;
		int64 Gpu;
	};

	// Compares one region and folds its output into the running digest, in the
	// bench's exact order: per column, the five column fields then that
	// column's whole vertical cell stack; then, after every column, the quads.
	// The order is load-bearing — a digest computed in a different order is
	// not comparable to the bench's, even when every byte matches.
	bool CompareRegion(const FRegionSpec& Region,
	                   const FVoxelGpuRegionRequest& Req,
	                   const FVoxelGpuRegionResult& Gpu,
	                   const TArray<vxc::ColumnSample>& CpuColumns,
	                   vxc::Digest& Digest,
	                   TArray<FMismatch>& OutMismatches)
	{
		constexpr int32 kMaxMismatchesPrinted = 20;
		const uint32 BricksX = Region.Width / 8;
		const uint32 BricksZ = Req.BricksZ;

		for (uint32 Y = 0; Y < Region.Height; ++Y)
		{
			for (uint32 X = 0; X < Region.Width; ++X)
			{
				const int32 ColIdx = int32(X + Y * Region.Width);
				const FVoxelGpuColumnSample& G = Gpu.Columns[ColIdx];
				const vxc::ColumnSample& C = CpuColumns[ColIdx];

				Digest.u32(static_cast<uint32_t>(G.SurfaceMm));
				Digest.u32(static_cast<uint32_t>(G.TopsoilMm));
				Digest.u32(static_cast<uint32_t>(G.SubsoilMm));
				Digest.u32(static_cast<uint32_t>(G.BedrockDepthMm));
				Digest.u8(static_cast<uint8_t>(G.SurfaceMat));

				const auto Record = [&](const TCHAR* Field, int64 CpuVal, int64 GpuVal)
				{
					if (CpuVal != GpuVal && OutMismatches.Num() < kMaxMismatchesPrinted)
					{
						OutMismatches.Add({ FString::Printf(TEXT("col(%d,%d).%s"),
						                                    int32(Region.OriginVx + X),
						                                    int32(Region.OriginVy + Y), Field),
						                    CpuVal, GpuVal });
					}
				};
				Record(TEXT("surfaceMm"), C.surfaceMm, G.SurfaceMm);
				Record(TEXT("topsoilMm"), C.topsoilMm, G.TopsoilMm);
				Record(TEXT("subsoilMm"), C.subsoilMm, G.SubsoilMm);
				Record(TEXT("bedrockDepthMm"), C.bedrockDepthMm, G.BedrockDepthMm);
				Record(TEXT("surfaceMat"), int64(C.surfaceMat), int64(G.SurfaceMat));

				// This column's vertical cell stack.
				const uint32 Bx = X / 8u, By = Y / 8u, Lx = X % 8u, Ly = Y % 8u;
				const uint32 FootprintIndex = Bx + BricksX * By;
				for (uint32 Bz = 0; Bz < BricksZ; ++Bz)
				{
					const uint32 BrickIndex = FootprintIndex * BricksZ + Bz;
					const int64 BrickZ = int64(Req.BrickZMin) + int64(Bz);
					for (uint32 ZLocal = 0; ZLocal < 8u; ++ZLocal)
					{
						const int64 Vz = BrickZ * 8 + ZLocal;
						const uint32 CellIdx = BrickIndex * 512 + CellIndexInBrick(Lx, Ly, ZLocal);
						const uint8 CpuMat = static_cast<uint8>(vxc::Amplifier::materialAt(C, Vz));
						const uint8 GpuMat = static_cast<uint8>(Gpu.Cells[CellIdx] & 0xffu);

						Digest.u8(GpuMat);
						if (CpuMat != GpuMat && OutMismatches.Num() < kMaxMismatchesPrinted)
						{
							OutMismatches.Add({ FString::Printf(TEXT("cell(%d,%d,vz=%lld)"),
							                                    int32(Region.OriginVx + X),
							                                    int32(Region.OriginVy + Y), Vz),
							                    CpuMat, GpuMat });
						}
					}
				}
			}
		}

		// --- quads ----------------------------------------------------------
		const uint32 InteriorX = BricksX - 2;
		const uint32 InteriorY = (Region.Height / 8) - 2;
		const uint32 InteriorZ = BricksZ - 2;

		std::vector<vxc::Quad> CpuQuads;
		uint32 GpuCursor = 0;

		for (uint32 Iz = 0; Iz < InteriorZ; ++Iz)
		{
			for (uint32 Iy = 0; Iy < InteriorY; ++Iy)
			{
				for (uint32 Ix = 0; Ix < InteriorX; ++Ix)
				{
					// Interior brick (ix,iy,iz) is region brick (ix+1,iy+1,iz+1):
					// the +1 skips the halo brick that supplies apron reads.
					const int64 Ox = (int64(Ix) + 1) * 8;
					const int64 Oy = (int64(Iy) + 1) * 8;
					const int64 Oz = (int64(Iz) + 1) * 8;

					const auto Sampler = [&](int Sx, int Sy, int Sz) -> vxc::MaterialId
					{
						const int64 Rvx = Ox + Sx;
						const int64 Rvy = Oy + Sy;
						const int64 Vz = int64(Req.BrickZMin) * 8 + Oz + Sz;
						const vxc::ColumnSample& C =
							CpuColumns[int32(Rvx + Rvy * int64(Region.Width))];
						return vxc::Amplifier::materialAt(C, Vz);
					};

					CpuQuads.clear();
					vxc::meshBrick<8>(Sampler, CpuQuads);

					for (const vxc::Quad& Q : CpuQuads)
					{
						const uint64 Packed = (GpuCursor < uint32(Gpu.Quads.Num()))
							? Gpu.Quads[int32(GpuCursor)]
							: ~0ull;
						const uint32 W0 = uint32(Packed & 0xffffffffull);
						const uint32 W1 = uint32(Packed >> 32);

						const uint8 GAxis  = uint8(W0 & 0xfu);
						const uint8 GDir   = uint8((W0 >> 4) & 0xfu);
						const uint8 GSlice = uint8((W0 >> 8) & 0xffu);
						const uint8 GU0    = uint8((W0 >> 16) & 0xffu);
						const uint8 GV0    = uint8((W0 >> 24) & 0xffu);
						const uint8 GW     = uint8(W1 & 0xffu);
						const uint8 GH     = uint8((W1 >> 8) & 0xffu);
						const uint8 GAo    = uint8((W1 >> 16) & 0xffu);
						const uint8 GMat   = uint8((W1 >> 24) & 0xffu);

						Digest.u8(GAxis);  Digest.u8(GDir);  Digest.u8(GSlice);
						Digest.u8(GU0);    Digest.u8(GV0);   Digest.u8(GW);
						Digest.u8(GH);     Digest.u8(GAo);   Digest.u8(GMat);

						const bool bSame = GAxis == Q.axis && GDir == Q.positive
						                && GSlice == Q.slice && GU0 == Q.u0 && GV0 == Q.v0
						                && GW == Q.w && GH == Q.h && GAo == Q.ao
						                && GMat == uint8(Q.mat);
						if (!bSame && OutMismatches.Num() < kMaxMismatchesPrinted)
						{
							OutMismatches.Add({
								FString::Printf(TEXT("quad[%u] brick(%u,%u,%u) cpu(ax%u d%u s%u ")
								                TEXT("u%u v%u w%u h%u ao%u m%u)"),
								                GpuCursor, Ix, Iy, Iz,
								                Q.axis, Q.positive, Q.slice, Q.u0, Q.v0,
								                Q.w, Q.h, Q.ao, uint32(Q.mat)),
								int64(Q.axis), int64(GAxis) });
						}
						++GpuCursor;
					}
				}
			}
		}

		if (GpuCursor != Gpu.NumQuads)
		{
			OutMismatches.Add({ TEXT("quad count"), int64(GpuCursor), int64(Gpu.NumQuads) });
		}

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("  [%s] %u columns, %u cells, %u quads (cpu %u)"),
		       Region.Name, Region.Width * Region.Height, Gpu.Cells.Num(), Gpu.NumQuads, GpuCursor);

		return OutMismatches.Num() == 0;
	}

	void VerifyRegionCommand(const TArray<FString>& Args)
	{
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("GPU worldgen needs SM6 (64-bit integer shader ops). Current feature ")
			       TEXT("level is lower. Relaunch the editor with -sm6, and make sure ")
			       TEXT("+D3D12TargetedShaderFormats=PCD3D_SM6 is in DefaultEngine.ini."));
			return;
		}

		// No argument runs both fixtures, which is the only mode whose digest
		// is comparable to the bench's.
		int32 Only = -1;
		if (Args.Num() > 0)
		{
			Only = FCString::Atoi(*Args[0]);
			if (Only < 0 || Only >= UE_ARRAY_COUNT(kRegions))
			{
				UE_LOG(LogVoxelGpuVerify, Error, TEXT("Region index must be 0..%d"),
				       int32(UE_ARRAY_COUNT(kRegions)) - 1);
				return;
			}
		}

		vxc::SyntheticTileSampler Tiles(kSeed);

		// A second, independent sampler for the CPU reference — the bench does
		// the same. Sharing one would let a stateful bug in the sampler cancel
		// itself out on both sides of the comparison.
		vxc::SyntheticTileSampler CpuTiles(kSeed);
		vxc::Amplifier CpuAmp(kSeed, CpuTiles);

		vxc::Digest Digest;
		TArray<FMismatch> Mismatches;
		bool bAllOk = true;
		double TotalMs = 0.0;

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("voxel.GPU.VerifyRegion: running the worldgen+mesher chain through Unreal's ")
		       TEXT("RDG, seed %llu"), kSeed);

		for (int32 R = 0; R < UE_ARRAY_COUNT(kRegions); ++R)
		{
			if (Only >= 0 && R != Only)
			{
				continue;
			}
			const FRegionSpec& Region = kRegions[R];

			TArray<vxc::ColumnSample> CpuColumns;
			const FVoxelGpuRegionRequest Req = BuildRequest(Region, CpuAmp, Tiles, CpuColumns);

			const double Start = FPlatformTime::Seconds();
			const FVoxelGpuRegionResult Gpu = VoxelGpuWorldGen::RunRegionBlocking(Req);
			const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
			TotalMs += Ms;

			if (!Gpu.bOk)
			{
				UE_LOG(LogVoxelGpuVerify, Error, TEXT("  [%s] dispatch FAILED: %s"),
				       Region.Name, *Gpu.Error);
				bAllOk = false;
				continue;
			}

			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("  [%s] origin (%d,%d) %ux%u columns, raster %ux%u px, bricks z [%d, %d], %.1f ms"),
			       Region.Name, Region.OriginVx, Region.OriginVy, Region.Width, Region.Height,
			       Req.RasterSize.X, Req.RasterSize.Y, Req.BrickZMin,
			       Req.BrickZMin + int32(Req.BricksZ) - 1, Ms);

			bAllOk &= CompareRegion(Region, Req, Gpu, CpuColumns, Digest, Mismatches);
		}

		// vxc::Digest exposes its FNV-1a accumulator directly as `h`.
		const uint64 Value = Digest.h;

		if (!Mismatches.IsEmpty())
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("First %d mismatch(es):"), Mismatches.Num());
			for (const FMismatch& M : Mismatches)
			{
				UE_LOG(LogVoxelGpuVerify, Error, TEXT("    %s: cpu=%lld gpu=%lld"),
				       *M.Where, M.Cpu, M.Gpu);
			}
		}

		if (bAllOk)
		{
			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("PASS: Unreal-compiled GPU output is bit-exact with the CPU reference ")
			       TEXT("(%.1f ms total)"), TotalMs);
		}
		else
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("FAIL: Unreal-compiled GPU output differs from the CPU reference"));
		}

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("Unreal GPU output digest (columns + cells + quads, combined): %016llx"), Value);

		if (Only < 0)
		{
			// Both fixtures, bench order. This is the cross-toolchain gate:
			// vxc_gpu.exe with no arguments must print this same number.
			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("Compare against: build/voxel-core-msvc/bench/vxc_gpu.exe (no args) — ")
			       TEXT("the digests must match exactly."));
		}
	}

	FAutoConsoleCommand GVoxelGpuVerifyRegionCmd(
		TEXT("voxel.GPU.VerifyRegion"),
		TEXT("Run the GPU worldgen+mesher chain through Unreal's RDG and byte-compare it against ")
		TEXT("the CPU reference. No args = both bench fixture regions (digest comparable to ")
		TEXT("vxc_gpu.exe); 0 or 1 = a single region."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyRegionCommand));
}
