#pragma once
// Sizing and filling the elevation/climate raster window a GPU region dispatch
// samples.
//
// THIS IS THE ONE PLACE THAT RULE MAY LIVE. FVoxelGpuRegionRequest's header
// spends a paragraph on it: the window must cover every pixel any thread in the
// dispatch can tap, the kernels clamp out-of-window reads deterministically
// rather than faulting, and a clamped read is silently different terrain from
// the CPU reference. A second copy of this arithmetic somewhere else is a bug
// waiting for someone to change one of them, so both the blocking digest gate
// (VoxelGpuVerify.cpp) and the async runner's harness call this.
//
// Includes voxel-core; keep it out of UHT-parsed headers.

#include "CoreMinimal.h"
#include "VoxelGpuWorldGen.h"

#include "voxelcore/core.h"
#include "voxelcore/caverns.h"
#include "voxelcore/tiles.h"   // vxc::ClimateSample, ITileSampler

namespace VoxelGpuRegionBuild
{
	// How far past the dispatch footprint the raster window must reach.
	//
	// This is NOT slack. VoxelizeMain's cavern pass evaluates the terrain
	// surface at a cave site's own xy, and a site can sit up to
	// kCavernMaxReachMm away from the column that queried it; the shader then
	// floors that to a voxel (one more voxel of reach) and takes bilinear taps
	// at px and px+1. Undersizing the window does not fault -- the kernel
	// clamps to the window edge -- it just silently produces different terrain
	// from the CPU.
	inline constexpr int64 kRasterCavernMarginMm = vxc::kCavernMaxReachMm + vxc::kVoxelSizeMm;

	// Fills PixelSizeMm / RasterOriginPx / RasterSize / ElevationMm /
	// ClimatePacked for the dispatch footprint already set on Req
	// (DispatchColumns + OriginVx/OriginVy). Everything else on Req is left
	// alone.
	//
	// Templated on the sampler so it works with vxc::SyntheticTileSampler and
	// with a real vxc::ITileSampler alike; both expose pixelSizeMm/elevationMm/
	// climate.
	template <typename TSampler>
	void FillRasterWindow(FVoxelGpuRegionRequest& Req, TSampler& Tiles)
	{
		const int64 PixelSizeMm = Tiles.pixelSizeMm();
		Req.PixelSizeMm = static_cast<int32>(PixelSizeMm);

		const int64 Width = int64(Req.DispatchColumns.X);
		const int64 Height = int64(Req.DispatchColumns.Y);

		const int64 XMmMin = int64(Req.OriginVx) * vxc::kVoxelSizeMm - kRasterCavernMarginMm;
		const int64 XMmMax = (int64(Req.OriginVx) + Width - 1) * vxc::kVoxelSizeMm
		                   + kRasterCavernMarginMm;
		const int64 YMmMin = int64(Req.OriginVy) * vxc::kVoxelSizeMm - kRasterCavernMarginMm;
		const int64 YMmMax = (int64(Req.OriginVy) + Height - 1) * vxc::kVoxelSizeMm
		                   + kRasterCavernMarginMm;

		const int64 PxMin = vxc::floorDiv(XMmMin, PixelSizeMm);
		const int64 PxMax = vxc::floorDiv(XMmMax, PixelSizeMm) + 1;  // +1: second bilinear tap
		const int64 PyMin = vxc::floorDiv(YMmMin, PixelSizeMm);
		const int64 PyMax = vxc::floorDiv(YMmMax, PixelSizeMm) + 1;

		const uint32 RasterW = static_cast<uint32>(PxMax - PxMin + 1);
		const uint32 RasterH = static_cast<uint32>(PyMax - PyMin + 1);

		Req.RasterOriginPx = FIntPoint(static_cast<int32>(PxMin), static_cast<int32>(PyMin));
		Req.RasterSize = FUintVector2(RasterW, RasterH);

		// Row-major, x fastest.
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
	}
}
