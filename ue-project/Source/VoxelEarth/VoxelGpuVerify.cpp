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
#include "VoxelGpuRegionBuild.h"

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

	// Builds one region's GPU request: the raster window, its contents, and the
	// z-range, exactly as gpu_harness.cpp::runRegion does.
	//
	// The raster window sizing itself lives in VoxelGpuRegionBuild.h so the async
	// runner's harness cannot get it subtly different from this proven one.
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

		VoxelGpuRegionBuild::FillRasterWindow(Req, Tiles);

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

	// CPU reference for the packed-quad decode.
	//
	// SCOPE, HONESTLY STATED: this is transcribed from BuildChunkVertexData in
	// VoxelChunkComponent.cpp, it is not that function. So it proves the shader
	// implements the SPEC — the bit unpacking, the corner order, the winding
	// flip, the AO bit remap, the level scale — which is where transcription
	// bugs actually live. It does NOT prove the GPU path matches the shipping
	// CPU renderer; only the G2 visual A/B against real terrain does that, and
	// that check is still owed.
	void DecodeQuadVertexCpu(uint64 Packed, uint32 CornerIndex, float LevelScale,
	                         float OutPosUU[3], uint32& OutAo, uint32& OutMat)
	{
		const uint32 W0 = uint32(Packed & 0xffffffffull);
		const uint32 W1 = uint32(Packed >> 32);

		const uint32 Axis     =  W0        & 0xfu;
		const uint32 Positive = (W0 >>  4) & 0xfu;
		const uint32 Slice    = (W0 >>  8) & 0xffu;
		const uint32 U0       = (W0 >> 16) & 0xffu;
		const uint32 V0       = (W0 >> 24) & 0xffu;
		const uint32 QW       =  W1        & 0xffu;
		const uint32 QH       = (W1 >>  8) & 0xffu;
		const uint32 Ao       = (W1 >> 16) & 0xffu;
		const uint32 Mat      = (W1 >> 24) & 0xffu;

		const uint32 U = (Axis + 1u) % 3u;
		const uint32 V = (Axis + 2u) % 3u;
		const float FaceCoordVox = float(Slice) + (Positive != 0u ? 1.0f : 0.0f);

		const float Us[4] = { float(U0), float(U0), float(U0 + QW), float(U0 + QW) };
		const float Vs[4] = { float(V0), float(V0 + QH), float(V0 + QH), float(V0) };

		static const uint32 ForwardWinding[6]  = { 0u, 1u, 2u, 0u, 2u, 3u };
		static const uint32 ReversedWinding[6] = { 0u, 2u, 1u, 0u, 3u, 2u };
		const uint32 Corner = (Positive != 0u) ? ForwardWinding[CornerIndex]
		                                       : ReversedWinding[CornerIndex];

		float PosVox[3] = { 0.f, 0.f, 0.f };
		PosVox[Axis] = FaceCoordVox;
		PosVox[U] = Us[Corner];
		PosVox[V] = Vs[Corner];

		// VoxelCoords::VoxelSizeUU
		constexpr float VoxelSizeUU = 10.0f;
		for (int32 I = 0; I < 3; ++I)
		{
			OutPosUU[I] = PosVox[I] * (VoxelSizeUU * LevelScale);
		}

		static const uint32 AoShiftForCorner[4] = { 0u, 4u, 6u, 2u };
		OutAo = (Ao >> AoShiftForCorner[Corner]) & 0x3u;
		OutMat = Mat;
	}

	// Compares the GPU decode against the CPU reference over a real quad
	// stream. Returns the number of mismatching vertices.
	int32 VerifyQuadDecode(const TArray<uint64>& Quads, float LevelScale)
	{
		TArray<VoxelGpuWorldGen::FDecodedVertex> GpuVerts;
		FString Error;
		if (!VoxelGpuWorldGen::DecodeQuadsBlocking(Quads, LevelScale, GpuVerts, Error))
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("  quad decode dispatch FAILED: %s"), *Error);
			return -1;
		}

		const int32 Expected = Quads.Num() * 6;
		if (GpuVerts.Num() != Expected)
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("  quad decode produced %d vertices, expected %d"),
			       GpuVerts.Num(), Expected);
			return -1;
		}

		int32 Mismatches = 0;
		for (int32 QuadIdx = 0; QuadIdx < Quads.Num(); ++QuadIdx)
		{
			for (uint32 Corner = 0; Corner < 6; ++Corner)
			{
				float CpuPos[3];
				uint32 CpuAo = 0, CpuMat = 0;
				DecodeQuadVertexCpu(Quads[QuadIdx], Corner, LevelScale, CpuPos, CpuAo, CpuMat);

				const VoxelGpuWorldGen::FDecodedVertex& G = GpuVerts[QuadIdx * 6 + int32(Corner)];

				// Positions are small integers times a power-of-two scale, so
				// they are exactly representable and an exact compare is
				// correct here — no epsilon, which would hide a real error.
				const bool bSame = G.PositionUU[0] == CpuPos[0]
				                && G.PositionUU[1] == CpuPos[1]
				                && G.PositionUU[2] == CpuPos[2]
				                && G.AmbientOcclusion == CpuAo
				                && G.MaterialId == CpuMat;
				if (!bSame)
				{
					if (Mismatches < 10)
					{
						UE_LOG(LogVoxelGpuVerify, Error,
						       TEXT("    quad %d corner %u: cpu (%.1f, %.1f, %.1f) ao=%u mat=%u ")
						       TEXT("vs gpu (%.1f, %.1f, %.1f) ao=%u mat=%u"),
						       QuadIdx, Corner, CpuPos[0], CpuPos[1], CpuPos[2], CpuAo, CpuMat,
						       G.PositionUU[0], G.PositionUU[1], G.PositionUU[2],
						       G.AmbientOcclusion, G.MaterialId);
					}
					++Mismatches;
				}
			}
		}
		return Mismatches;
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

			// G2 decode check, over this region's real quad stream. Level 0,
			// so LevelScale is 1.
			const int32 DecodeMismatches = VerifyQuadDecode(Gpu.Quads, 1.0f);
			if (DecodeMismatches < 0)
			{
				bAllOk = false;
			}
			else if (DecodeMismatches > 0)
			{
				UE_LOG(LogVoxelGpuVerify, Error,
				       TEXT("  [%s] quad decode: %d of %d vertices differ from the CPU reference"),
				       Region.Name, DecodeMismatches, Gpu.Quads.Num() * 6);
				bAllOk = false;
			}
			else
			{
				UE_LOG(LogVoxelGpuVerify, Log,
				       TEXT("  [%s] quad decode: %d vertices match the CPU reference exactly"),
				       Region.Name, Gpu.Quads.Num() * 6);
			}
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

// ---------------------------------------------------------------------------
// voxel.GPU.SpawnTestChunk — the G2 visual check
//
// Everything up to here is verified numerically: the kernels are bit-exact vs
// the CPU mesher, and the quad decode matches a CPU reference exactly. What no
// number can answer is whether the DRAW path works -- whether the geometry is
// lit, shaded, oriented and wound correctly once it reaches the renderer.
//
// So this meshes a region on the GPU and hangs the result in the air in front
// of the player, drawn entirely by FVoxelQuadVertexFactory: no vertex buffer,
// no index buffer, every corner rebuilt from SV_VertexID.
//
// It is deliberately floating and offset rather than sitting in the terrain --
// against open sky a wrong winding or a flipped normal is obvious, whereas
// buried in the landscape it would just look like more ground.
// ---------------------------------------------------------------------------

#include "VoxelGpuChunkComponent.h"
#include "VoxelGpuPoolComponent.h"
#include "VoxelClimateProbe.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "DrawDebugHelpers.h"
#include "VoxelChunkComponent.h"
#include "VoxelMeshTypes.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "UnrealClient.h"
#include "TimerManager.h"

namespace
{
	// Re-bases packed quads from BRICK-LOCAL to region-local coordinates.
	//
	// greedyMask packs slice/u0/v0 inside a single 8x8x8 brick and leaves the
	// brick's origin implied by the mask index, so the raw stream piles all
	// bricks into one 8-voxel cube. The CPU mesher does this same re-basing
	// when it converts vxc::Quad to FVoxelChunkQuad.
	//
	// This is a G2 stopgap: fine for a test chunk, but G3 must do it GPU-side
	// (or have MeshEmit bake it) rather than round-tripping quads through the
	// CPU, which is exactly what ADR-0006 is trying to stop doing.
	TArray<uint64> RebaseQuadsToRegionLocal(const FVoxelGpuRegionResult& Gpu,
	                                        uint32 BricksX, uint32 BricksY)
	{
		const uint32 InteriorX = BricksX - 2;
		const uint32 InteriorY = BricksY - 2;

		TArray<uint64> Rebased;
		Rebased.Reserve(Gpu.Quads.Num());

		for (int32 MaskIndex = 0; MaskIndex < Gpu.QuadCounts.Num(); ++MaskIndex)
		{
			const uint32 Count = Gpu.QuadCounts[MaskIndex];
			if (Count == 0)
			{
				continue;
			}
			const uint32 Start = Gpu.QuadOffsets[MaskIndex];

			// maskIndex = meshBrickIndex * 48 + axis * 16 + dir * 8 + slice
			const uint32 MeshBrickIndex = uint32(MaskIndex) / 48u;
			const uint32 Ix = MeshBrickIndex % InteriorX;
			const uint32 Iy = (MeshBrickIndex / InteriorX) % InteriorY;
			const uint32 Iz = MeshBrickIndex / (InteriorX * InteriorY);

			// Interior brick (ix,iy,iz) is region brick (ix+1, iy+1, iz+1).
			const uint32 BrickOrigin[3] = { (Ix + 1u) * 8u, (Iy + 1u) * 8u, (Iz + 1u) * 8u };

			for (uint32 Q = 0; Q < Count; ++Q)
			{
				const uint64 Packed = Gpu.Quads[int32(Start + Q)];
				const uint32 W0 = uint32(Packed & 0xffffffffull);
				const uint32 W1 = uint32(Packed >> 32);

				const uint32 Axis  =  W0        & 0xfu;
				const uint32 Dir   = (W0 >>  4) & 0xfu;
				const uint32 Slice = (W0 >>  8) & 0xffu;
				const uint32 U0    = (W0 >> 16) & 0xffu;
				const uint32 V0    = (W0 >> 24) & 0xffu;

				const uint32 U = (Axis + 1u) % 3u;
				const uint32 V = (Axis + 2u) % 3u;

				const uint32 NewSlice = Slice + BrickOrigin[Axis];
				const uint32 NewU0    = U0    + BrickOrigin[U];
				const uint32 NewV0    = V0    + BrickOrigin[V];

				const uint32 NewW0 = Axis | (Dir << 4) | (NewSlice << 8)
				                   | (NewU0 << 16) | (NewV0 << 24);
				Rebased.Add(uint64(NewW0) | (uint64(W1) << 32));
			}
		}
		return Rebased;
	}

	void SpawnTestChunkCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("No world"));
			return;
		}
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("Needs SM6. Relaunch with -sm6."));
			return;
		}

		// Mesh the origin fixture on the GPU -- the same region the digest gate
		// uses, so its contents are already known-good.
		vxc::SyntheticTileSampler Tiles(kSeed);
		vxc::SyntheticTileSampler CpuTiles(kSeed);
		vxc::Amplifier CpuAmp(kSeed, CpuTiles);

		TArray<vxc::ColumnSample> CpuColumns;
		const FVoxelGpuRegionRequest Req = BuildRequest(kRegions[0], CpuAmp, Tiles, CpuColumns);
		const FVoxelGpuRegionResult Gpu = VoxelGpuWorldGen::RunRegionBlocking(Req);

		if (!Gpu.bOk)
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("GPU mesh failed: %s"), *Gpu.Error);
			return;
		}
		if (Gpu.Quads.IsEmpty())
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("GPU produced no quads — nothing to draw"));
			return;
		}

		const TArray<uint64> Rebased = RebaseQuadsToRegionLocal(
			Gpu, kRegions[0].Width / 8, kRegions[0].Height / 8);

		// Positioned from the CAMERA, not the pawn.
		//
		// The pawn's actor forward is not necessarily where the player is
		// looking -- on a fly pawn the view can be rotated independently -- so
		// "pawn location + actor forward" can put the chunk cleanly off-screen,
		// which is indistinguishable from it failing to render. GetPlayerViewPoint
		// is what the player actually sees.
		//
		// Lifted well above the view direction too, so it reads against open sky
		// instead of being buried in the hillside the camera is pointed at.
		FVector SpawnLocation = FVector::ZeroVector;
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector CamLoc = FVector::ZeroVector;
			FRotator CamRot = FRotator::ZeroRotator;
			PC->GetPlayerViewPoint(CamLoc, CamRot);
			SpawnLocation = CamLoc + CamRot.Vector() * 2000.0 + FVector(0, 0, -700);
		}

		// CONTROL EXPERIMENT: "voxel.GPU.SpawnTestChunk control" puts an ordinary
		// engine cube at the SAME place instead of the GPU chunk.
		//
		// Worth its few lines. Everything so far has assumed the test rig is
		// sound -- that the spawn point is in view, lit, and not inside a hill.
		// That assumption has never been checked, and if it is wrong then every
		// "invisible" result so far says nothing about the draw path. A stock
		// static mesh either shows up or it does not, and either answer is
		// worth more than another round of guessing at the renderer.
		const bool bControl = Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("control"), ESearchCase::IgnoreCase); });
		if (bControl)
		{
			AActor* ControlActor = World->SpawnActor<AActor>(
				AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator);
			UStaticMeshComponent* SM = NewObject<UStaticMeshComponent>(ControlActor);
			ControlActor->SetRootComponent(SM);
			// Try both cube paths and SAY which one worked. The first version of
			// this control silently did nothing when the mesh failed to load,
			// which would have made "the control is invisible too" a false
			// negative -- the worst possible outcome for a control experiment.
			UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (Cube == nullptr)
			{
				Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EngineMeshes/Cube.Cube"));
			}
			UE_LOG(LogVoxelGpuVerify, Log, TEXT("CONTROL: cube mesh %s"),
			       Cube ? *Cube->GetPathName() : TEXT("FAILED TO LOAD"));
			if (Cube)
			{
				SM->SetStaticMesh(Cube);
				SM->SetWorldScale3D(FVector(20.0));   // 20 m cube
			}
			SM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			SM->RegisterComponent();
			// MUST come after RegisterComponent. SetRootComponent on a freshly
			// NewObject'd component installs it with an IDENTITY transform, and
			// since the root component is what defines the actor's location,
			// that silently teleports the actor to the world origin -- 8,448 km
			// from the camera in this test.
			SM->SetWorldLocation(SpawnLocation);
			// Second, asset-free control: a persistent debug box. It goes through
			// an entirely different rendering path from static meshes, so if the
			// box appears and the cube does not, the fault is in the mesh/asset
			// side; if NEITHER appears, the location itself is not visible.
			DrawDebugBox(World, SpawnLocation, FVector(1000.0), FColor::Red, true, -1.0f, 0, 50.0f);
			DrawDebugSphere(World, SpawnLocation, 800.0f, 16, FColor::Yellow, true, -1.0f, 0, 40.0f);

			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("CONTROL: stock cube + debug box/sphere at %s. If NONE of them are ")
			       TEXT("visible, the test rig is at fault, not the GPU draw path."),
			       *SpawnLocation.ToString());
		}

		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator);
		if (Actor == nullptr)
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("Failed to spawn actor"));
			return;
		}
		Actor->SetActorLabel(TEXT("VoxelGpuTestChunk"));

		// The real terrain material, so the A/B compares like with like. Without
		// it the chunk falls back to WorldGridMaterial and every difference is
		// swamped by "one is grey".
		UMaterialInterface* TerrainMaterial = Cast<UMaterialInterface>(StaticLoadObject(
			UMaterialInterface::StaticClass(), nullptr,
			TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain")));
		UE_LOG(LogVoxelGpuVerify, Log, TEXT("Terrain material: %s"),
		       TerrainMaterial ? *TerrainMaterial->GetPathName() : TEXT("NOT FOUND (falling back to grey)"));

		UVoxelGpuChunkComponent* Comp = NewObject<UVoxelGpuChunkComponent>(Actor);
		Actor->SetRootComponent(Comp);
		Comp->SetChunkLevel(0);
		Comp->SetChunkMaterial(TerrainMaterial);
		Comp->SetQuads(Rebased);
		Comp->RegisterComponent();
		// See the control above: without this the chunk sits at the world
		// origin regardless of where the actor was spawned.
		Comp->SetWorldLocation(SpawnLocation);

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("Spawned a GPU-drawn chunk at %s: %d quads, %d triangles, drawn with no vertex ")
		       TEXT("or index buffer."),
		       *SpawnLocation.ToString(), Rebased.Num(), Rebased.Num() * 2);

		// THE ACTUAL G2 GATE: the same quads, drawn by the shipping CPU path,
		// placed alongside. Feeding both renderers identical geometry is what
		// makes the comparison mean something -- any difference is the DRAW
		// path, since the mesher input is byte-identical by construction.
		if (Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("ab"), ESearchCase::IgnoreCase); }))
		{
			TArray<FVoxelChunkQuad> CpuQuads;
			CpuQuads.Reserve(Rebased.Num());
			for (const uint64 Packed : Rebased)
			{
				const uint32 W0 = uint32(Packed & 0xffffffffull);
				const uint32 W1 = uint32(Packed >> 32);
				FVoxelChunkQuad Q;
				Q.Axis     = uint8( W0        & 0xfu);
				Q.Positive = uint8((W0 >>  4) & 0xfu);
				Q.Slice    = uint8((W0 >>  8) & 0xffu);
				Q.U0       = uint8((W0 >> 16) & 0xffu);
				Q.V0       = uint8((W0 >> 24) & 0xffu);
				Q.W        = uint8( W1        & 0xffu);
				Q.H        = uint8((W1 >>  8) & 0xffu);
				Q.Ao       = uint8((W1 >> 16) & 0xffu);
				Q.Mat      = uint8((W1 >> 24) & 0xffu);
				CpuQuads.Add(Q);
			}

			// Offset sideways so both are in frame at once. 800 UU = 8 m, a bit
			// wider than the 6.4 m region, so they sit adjacent without overlap.
			const FVector CpuLocation = SpawnLocation + FVector(0, 800, 0);
			AActor* CpuActor = World->SpawnActor<AActor>(
				AActor::StaticClass(), CpuLocation, FRotator::ZeroRotator);
			UVoxelChunkComponent* CpuComp = NewObject<UVoxelChunkComponent>(CpuActor);
			CpuActor->SetRootComponent(CpuComp);
			if (TerrainMaterial)
			{
				CpuComp->SetMaterial(0, TerrainMaterial);
			}
			// 64, not 32: this is a region, not one chunk.
			CpuComp->SetChunkQuads(MoveTemp(CpuQuads), 64);
			CpuComp->RegisterComponent();
			CpuComp->SetWorldLocation(CpuLocation);

			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("A/B: CPU-meshed chunk from the SAME quads at %s (GPU on the left, "
			            "CPU 8 m to its right). They should be indistinguishable."),
			       *CpuLocation.ToString());
		}

		// Optional self-check: "voxel.GPU.SpawnTestChunk shot" grabs a
		// screenshot a couple of seconds later.
		//
		// The delay is the point. The component only marks its render state
		// dirty here; the scene proxy is built, its RHI resources created and
		// the first frame drawn some frames afterwards. Shooting immediately
		// would reliably capture an empty sky and read as a failure.
		//
		// This exists so the visual half of the G2 gate can be inspected from a
		// headless run instead of costing someone a play session.
		const bool bWantScreenshot = Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("shot"), ESearchCase::IgnoreCase); });
		if (bWantScreenshot)
		{
			FTimerHandle Handle;
			TWeakObjectPtr<UWorld> WeakWorld(World);
			const FVector SpawnedAt = SpawnLocation;
			World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakWorld, SpawnedAt]()
			{
				// Log where the camera is NOW, not where it was at spawn time.
				// The control cube being invisible proves the rig is wrong, and
				// the likeliest reason is that the view moves between the
				// command running at startup and the screenshot 3 s later --
				// leaving everything spawned 20 m from where the camera used
				// to be.
				if (UWorld* W = WeakWorld.Get())
				{
					if (APlayerController* PC = W->GetFirstPlayerController())
					{
						FVector CamLoc = FVector::ZeroVector;
						FRotator CamRot = FRotator::ZeroRotator;
						PC->GetPlayerViewPoint(CamLoc, CamRot);
						UE_LOG(LogVoxelGpuVerify, Log,
						       TEXT("AT SCREENSHOT: camera %s rot %s | spawned at %s | distance %.0f UU"),
						       *CamLoc.ToString(), *CamRot.ToString(), *SpawnedAt.ToString(),
						       FVector::Dist(CamLoc, SpawnedAt));
					}
				}
				FScreenshotRequest::RequestScreenshot(false);
				UE_LOG(LogVoxelGpuVerify, Log, TEXT("Screenshot requested (Saved/Screenshots/)"));
			}), 3.0f, false);
		}
	}

	FAutoConsoleCommandWithWorldAndArgs GVoxelGpuSpawnTestChunkCmd(
		TEXT("voxel.GPU.SpawnTestChunk"),
		TEXT("Mesh a region on the GPU and draw it in front of the player through the GPU vertex "
		     "factory (no vertex/index buffer). The G2 visual check."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpawnTestChunkCommand));
}


// ---------------------------------------------------------------------------
// voxel.GPU.SpawnPool <N> — the ADR-0006 shape, demonstrated
//
// Puts N chunks into ONE pool, drawn by ONE primitive in ONE draw call. The
// single-chunk command proves geometry is correct; this proves the thing the
// ADR is actually for: that how many chunks are resident has no bearing on how
// many primitives or draws the renderer sees.
//
// It meshes ONE region on the GPU and places that geometry at N different
// origins. Re-meshing N distinct regions would cost N times the setup and
// prove nothing extra about the DRAW path, which is what is under test here.
// ---------------------------------------------------------------------------

namespace
{
	// Climate for one chunk, sampled at its world centre.
	//
	// The CPU path samples per QUAD; this samples per CHUNK. A chunk is 3.2 m
	// across a 30 m climate raster cell -- roughly 1/100th of a pixel's area --
	// and climate varies as a smooth bilinear ramp over that distance, so the
	// difference is a gentle chunk-to-chunk gradient rather than banding. The
	// CPU path's own comment already describes its per-quad sampling as ~10x
	// oversampling the source raster.
	//
	// Returns 0..1 temperature/precipitation, matching what the CPU path writes
	// into vertex colour B/A for T_VoxelBiomeLUT.
	FVector4f SampleChunkClimate(const FVector& PoolWorldOrigin, const FVector3f& ChunkOriginUU)
	{
		// Chunk centre: the region is 64 voxels, so 320 UU in from its origin.
		const double CentreX = PoolWorldOrigin.X + double(ChunkOriginUU.X) + 320.0;
		const double CentreY = PoolWorldOrigin.Y + double(ChunkOriginUU.Y) + 320.0;

		VoxelClimate::EnsureInitialized();
		const FVoxelClimateBytes Bytes = VoxelClimate::SampleClimateAtWorldUU(CentreX, CentreY);
		// Z is left at "no surface gate" deliberately. This pool is the
		// known-good CONTROL (docs/gpu-pool-rendering-notes.md: "run the
		// known-good test pool in the same process, same frame, same material as
		// the failing one"), so its image has to stay comparable to every earlier
		// screenshot of it. Gating it would change the reference for a reason
		// that has nothing to do with what the control tests.
		return FVector4f(float(Bytes.Temperature) / 255.0f,
		                 float(Bytes.Precipitation) / 255.0f,
		                 UVoxelGpuPoolComponent::kNoSurfaceGate,
		                 0.0f);
	}

	void SpawnPoolCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr || !VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("No world, or SM6 unavailable."));
			return;
		}

		const int32 NumChunks = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 4096) : 64;

		vxc::SyntheticTileSampler Tiles(kSeed);
		vxc::SyntheticTileSampler CpuTiles(kSeed);
		vxc::Amplifier CpuAmp(kSeed, CpuTiles);

		TArray<vxc::ColumnSample> CpuColumns;
		const FVoxelGpuRegionRequest Req = BuildRequest(kRegions[0], CpuAmp, Tiles, CpuColumns);
		const FVoxelGpuRegionResult Gpu = VoxelGpuWorldGen::RunRegionBlocking(Req);
		if (!Gpu.bOk || Gpu.Quads.IsEmpty())
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("GPU mesh failed: %s"), *Gpu.Error);
			return;
		}

		const TArray<uint64> Rebased = RebaseQuadsToRegionLocal(Gpu, kRegions[0].Width / 8, kRegions[0].Height / 8);

		FVector SpawnLocation = FVector::ZeroVector;
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector CamLoc; FRotator CamRot;
			PC->GetPlayerViewPoint(CamLoc, CamRot);
			SpawnLocation = CamLoc + CamRot.Vector() * 3000.0 + FVector(0, 0, -900);
		}

		UMaterialInterface* TerrainMaterial = Cast<UMaterialInterface>(StaticLoadObject(
			UMaterialInterface::StaticClass(), nullptr,
			TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain")));

		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator);
		UVoxelGpuPoolComponent* Pool = NewObject<UVoxelGpuPoolComponent>(Actor);
		Actor->SetRootComponent(Pool);
		Pool->SetChunkMaterial(TerrainMaterial);

		// Capacity with headroom, so churn has somewhere to go.
		Pool->InitPool(uint32(Rebased.Num()) * uint32(NumChunks) * 2u);

		// Lay the chunks out in a grid. 640 UU = 6.4 m is the region edge, so
		// they tile without overlapping.
		const int32 Side = FMath::CeilToInt(FMath::Sqrt(double(NumChunks)));
		TArray<int32> Handles;
		for (int32 I = 0; I < NumChunks; ++I)
		{
			const int32 Gx = I % Side;
			const int32 Gy = I / Side;
			const FVector3f Origin(float(Gx) * 640.0f, float(Gy) * 640.0f, 0.0f);
			Handles.Add(Pool->AddChunk(Rebased, Origin, 0,
				SampleChunkClimate(SpawnLocation, Origin)));
		}

		// CHURN: drop every other chunk and re-add half of them. This is what
		// streaming actually does, and the point is that none of it touches
		// FScene -- the primitive count is 1 before, during and after.
		if (Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("churn"), ESearchCase::IgnoreCase); }))
		{
			int32 Removed = 0;
			for (int32 I = 0; I < Handles.Num(); I += 2)
			{
				if (Handles[I] != INDEX_NONE)
				{
					Pool->RemoveChunk(Handles[I]);
					++Removed;
				}
			}
			int32 ReAdded = 0;
			for (int32 I = 0; I < Handles.Num() / 4; ++I)
			{
				const int32 Gx = I % Side;
				const int32 Gy = I / Side;
				const FVector3f ReAddOrigin(float(Gx) * 640.0f, float(Gy) * 640.0f, 900.0f);
				if (Pool->AddChunk(Rebased, ReAddOrigin, 0,
					SampleChunkClimate(SpawnLocation, ReAddOrigin)) != INDEX_NONE)
				{
					++ReAdded;
				}
			}
			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("Churn: removed %d, re-added %d — %d live chunks, %u quads used, "
			            "%u free, largest run %u"),
			       Removed, ReAdded, Pool->GetNumChunks(), Pool->GetHighWaterMarkQuads(),
			       Pool->GetFreeQuads(), Pool->GetLargestFreeRun());
		}

		Pool->RegisterComponent();
		Pool->SetWorldLocation(SpawnLocation);

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("Pool: %d live chunks, %u quads used, %d triangles in ONE primitive at %s"),
		       Pool->GetNumChunks(), Pool->GetHighWaterMarkQuads(),
		       int32(Pool->GetHighWaterMarkQuads()) * 2, *SpawnLocation.ToString());

		// "churnlive" is the mode that actually tests G3.
		//
		// Plain "churn" adds and churns inside this one console command, so the
		// scene proxy does not exist yet for any of it and every edit collapses
		// into a single full rebuild at end of frame. The incremental upload
		// path -- the thing streaming will lean on constantly -- never runs, and
		// a bug in it renders a perfectly clean screenshot. That is precisely
		// how a wrong buffer usage flag survived verification once already.
		//
		// Deferring the churn by a few seconds puts it after the proxy is live,
		// so RemoveChunk/AddChunk/UpdateChunk go through PushUpdatesToProxy and
		// write real sub-ranges of a real GPU buffer.
		if (Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("churnlive"), ESearchCase::IgnoreCase); }))
		{
			TWeakObjectPtr<UVoxelGpuPoolComponent> WeakPool(Pool);
			FTimerHandle ChurnHandle;
			World->GetTimerManager().SetTimer(ChurnHandle, FTimerDelegate::CreateLambda(
				[WeakPool, Handles, Rebased, Side, SpawnLocation]()
			{
				UVoxelGpuPoolComponent* LivePool = WeakPool.Get();
				if (LivePool == nullptr)
				{
					return;
				}

				int32 Removed = 0;
				for (int32 I = 0; I < Handles.Num(); I += 2)
				{
					if (Handles[I] != INDEX_NONE)
					{
						LivePool->RemoveChunk(Handles[I]);
						++Removed;
					}
				}

				// Re-add into the runs just freed, one Z-step up so the result is
				// visually unambiguous: anything still drawing at the old height
				// is a stale range that was not actually overwritten.
				int32 ReAdded = 0;
				for (int32 I = 0; I < Handles.Num() / 4; ++I)
				{
					const FVector3f ReAddOrigin(
						float(I % Side) * 640.0f, float(I / Side) * 640.0f, 900.0f);
					if (LivePool->AddChunk(Rebased, ReAddOrigin, 0,
						SampleChunkClimate(SpawnLocation, ReAddOrigin)) != INDEX_NONE)
					{
						++ReAdded;
					}
				}

				// And re-mesh a survivor in place, which is the case G3 hits on
				// every dig: same handle, same slot, rewritten contents.
				int32 Updated = 0;
				for (int32 I = 1; I < Handles.Num(); I += 8)
				{
					if (Handles[I] != INDEX_NONE &&
					    LivePool->UpdateChunk(Handles[I], Rebased) != INDEX_NONE)
					{
						++Updated;
					}
				}

				UE_LOG(LogVoxelGpuVerify, Log,
				       TEXT("Live churn (proxy already up): removed %d, re-added %d, "
				            "updated-in-place %d — %d live chunks, %u high water, %u free"),
				       Removed, ReAdded, Updated, LivePool->GetNumChunks(),
				       LivePool->GetHighWaterMarkQuads(), LivePool->GetFreeQuads());
			}), 5.0f, false);
		}

		if (Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("shot"), ESearchCase::IgnoreCase); }))
		{
			FTimerHandle Handle;
			// 10 s, not 3. The pool is spawned into the live streamed world, so
			// a shot taken before the CPU cascade has filled shows half-loaded
			// terrain around the pool and invites blaming the pool for it --
			// which is exactly what happened once.
			World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([]()
			{
				FScreenshotRequest::RequestScreenshot(false);
			}), 10.0f, false);
		}
	}

	// Screenshot the live game N seconds from now.
	//
	// Exists because the interesting question is usually "what does the normal
	// streamed world look like under some cvar", and every screenshot harness
	// before this was welded to a spawn command. Headless runs pair it with
	// -ExecCmds, e.g. "voxel.Stream.GPU 1, voxel.Debug.ShotIn 25" -- long
	// enough for the cascade to fill, since a shot taken mid-fill shows coarse
	// LOD and reads as a rendering bug.
	void ShotInCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}
		const float Delay = (Args.Num() > 0) ? FMath::Max(0.1f, FCString::Atof(*Args[0])) : 20.0f;
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([]()
		{
			FScreenshotRequest::RequestScreenshot(false);
			UE_LOG(LogVoxelGpuVerify, Log, TEXT("Screenshot requested (Saved/Screenshots/)"));
		}), Delay, false);
		UE_LOG(LogVoxelGpuVerify, Log, TEXT("Screenshot scheduled in %.1f s"), Delay);
	}

	FAutoConsoleCommandWithWorldAndArgs GVoxelDebugShotInCmd(
		TEXT("voxel.Debug.ShotIn"),
		TEXT("Take a screenshot N seconds from now (default 20). Usage: voxel.Debug.ShotIn [seconds]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ShotInCommand));

	FAutoConsoleCommandWithWorldAndArgs GVoxelGpuSpawnPoolCmd(
		TEXT("voxel.GPU.SpawnPool"),
		TEXT("Put N chunks in one GPU pool drawn by ONE primitive in ONE draw call. "
		     "Usage: voxel.GPU.SpawnPool [N] [churn|churnlive] [shot]. "
		     "churn edits before the proxy exists (one rebuild); churnlive edits "
		     "after it is up, which is the path streaming actually uses."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpawnPoolCommand));
}
