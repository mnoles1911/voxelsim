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

	// The window a dispatch can tap, in tile-pixel coordinates, inclusive on
	// both ends. Produced ONLY by ComputeRasterWindowPx below.
	struct FRasterWindowPx
	{
		int64 PxMin = 0;
		int64 PxMax = -1;
		int64 PyMin = 0;
		int64 PyMax = -1;
	};

	// THE WINDOW RULE, AS A PURE FUNCTION -- the single spelling of "which
	// pixels can this dispatch tap". FillRasterWindow (below) consumes it to
	// build the per-request inline window; the raster-atlas prefetch
	// (FVoxelRasterAtlasCpu) consumes the SAME function to size the resident
	// margin and to coverage-check each atlas-armed request before submit.
	// Splitting the arithmetic out is what lets the atlas exist WITHOUT
	// becoming the second copy this header's warning is about: an atlas that
	// re-derived the margin independently would drift from the verify
	// harness's window exactly the way the D5 note below describes, and would
	// fail the same silent way.
	inline FRasterWindowPx ComputeRasterWindowPx(int32 OriginVx, int32 OriginVy,
	                                             FUintVector2 DispatchColumns,
	                                             int32 CoarseLevel, int64 PixelSizeMm)
	{
		const int64 Width = int64(DispatchColumns.X);
		const int64 Height = int64(DispatchColumns.Y);

		// D5. AT LEVEL L THE DISPATCH INDICES ARE COARSE CELLS, AND THE KERNEL
		// SAMPLES AT THEIR REPRESENTATIVE LEVEL-0 COORDINATE -- so the window a
		// level-L dispatch touches is 2^L times wider than its column count
		// suggests. Sizing it in level-0 voxels, as this did, produced a window
		// that was correct at level 0 and far too narrow at every level above.
		//
		// AND IT FAILED EXACTLY THE WAY THE HEADER ABOVE SAYS IT WOULD: not a
		// fault, not an error, but reads clamped to the window edge -- silently
		// different terrain from the CPU. The D5 per-level gate caught it on its
		// first run, and the signature is worth recording because it is what
		// distinguishes this from a coarse-arithmetic bug: the [origin] fixture
		// passed columns AND cells at level 1 (small coordinates, so the margin
		// still covered the span) while [far-negative] reported 12,288 column
		// and 108,470 cell mismatches. A bug in coarseRep itself would have
		// failed both equally.
		//
		// Clamp ceiling 6 since the 8.19 km ring landed (2026-08-23), matching
		// FillLooseParameters' CoarseScale clamp -- a level the cascade streams
		// but this function refused would size a level-6 window as level-5's,
		// half as wide as the kernel samples: the D5 failure again, one level up.
		const int64 CoarseScale = int64(1) << FMath::Clamp(CoarseLevel, 0, 6);
		const auto CoarseRepMm = [CoarseScale](int64 Cell)
		{
			// Same two operations as vxc::coarseRep and worldgen.ush's, then to
			// millimetres. Identity at level 0.
			return (Cell * CoarseScale + CoarseScale / 2) * vxc::kVoxelSizeMm;
		};

		const int64 XMmMin = CoarseRepMm(int64(OriginVx)) - kRasterCavernMarginMm;
		const int64 XMmMax = CoarseRepMm(int64(OriginVx) + Width - 1) + kRasterCavernMarginMm;
		const int64 YMmMin = CoarseRepMm(int64(OriginVy)) - kRasterCavernMarginMm;
		const int64 YMmMax = CoarseRepMm(int64(OriginVy) + Height - 1) + kRasterCavernMarginMm;

		// v9: the carrier is a cubic B-spline, so a column in cell px reads
		// control points px-1..px+2 rather than bilinear's px..px+1. The margins
		// come from vxc::kCarrierStencilLo/Hi rather than literals precisely
		// because getting this wrong does not fault -- rasterElevationMm clamps
		// to the window, so an under-sized window silently produces different
		// terrain on the GPU than on the CPU. That is the same failure the D5
		// note above records.
		FRasterWindowPx W;
		W.PxMin = vxc::floorDiv(XMmMin, PixelSizeMm) + vxc::kCarrierStencilLo;
		W.PxMax = vxc::floorDiv(XMmMax, PixelSizeMm) + vxc::kCarrierStencilHi;
		W.PyMin = vxc::floorDiv(YMmMin, PixelSizeMm) + vxc::kCarrierStencilLo;
		W.PyMax = vxc::floorDiv(YMmMax, PixelSizeMm) + vxc::kCarrierStencilHi;
		return W;
	}

	// Fills PixelSizeMm / RasterOriginPx / RasterSize / ElevationMm /
	// ClimatePacked for the dispatch footprint already set on Req
	// (DispatchColumns + OriginVx/OriginVy). Everything else on Req is left
	// alone.
	//
	// Templated on the sampler so it works with vxc::SyntheticTileSampler and
	// with a real vxc::ITileSampler alike; both expose pixelSizeMm/elevationMm/
	// climate.
	//
	// UNDER -VoxelGpuRasterAtlas THE HOT PATH NEVER CALLS THIS. The window
	// arithmetic it applies (ComputeRasterWindowPx above) is still consulted
	// per request -- as a coverage CHECK against the resident atlas rather
	// than as a 5,800-pixel fill -- and this function survives as the verify
	// harnesses' reference and as the control arm. One rule, two consumers,
	// zero copies.
	template <typename TSampler>
	void FillRasterWindow(FVoxelGpuRegionRequest& Req, TSampler& Tiles)
	{
		const int64 PixelSizeMm = Tiles.pixelSizeMm();
		Req.PixelSizeMm = static_cast<int32>(PixelSizeMm);

		const FRasterWindowPx W = ComputeRasterWindowPx(
			Req.OriginVx, Req.OriginVy, Req.DispatchColumns, Req.CoarseLevel, PixelSizeMm);
		const int64 PxMin = W.PxMin;
		const int64 PyMin = W.PyMin;

		const uint32 RasterW = static_cast<uint32>(W.PxMax - W.PxMin + 1);
		const uint32 RasterH = static_cast<uint32>(W.PyMax - W.PyMin + 1);

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
