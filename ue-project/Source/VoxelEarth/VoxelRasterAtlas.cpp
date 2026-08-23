// FVoxelRasterAtlasCpu -- policy side of the persistent GPU raster atlas.
// See the header for the design and the failing readings; this file is the
// mechanics: margin derivation THROUGH the one window rule, nearest-first
// fills under a time budget, the coverage check, and the stats window.

#include "VoxelRasterAtlas.h"

#include "VoxelGpuWorldGen.h"

#include "voxelcore/core.h"

#include "Misc/CommandLine.h"
#include "RenderingThread.h"   // ENQUEUE_RENDER_COMMAND (Shutdown)
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelRasterAtlas, Log, All);

namespace
{
	// Stats cadence, matching the streaming counters' 5 s windows so the
	// atlas line lands beside the lines it will be read against.
	constexpr double kWindowSeconds = 5.0;

	// Per-tick fill budget in milliseconds of game-thread sampler time.
	// A page is ~0.3-1.4 ms (16,384 samples at FillRasterWindow's measured
	// 12-58 Mpx/s single-thread rate), so the default 2.0 admits 1-6 pages a
	// tick: ~120 pages/s at 60 fps against the ~70/s that 240 m/s flight
	// consumes, and a cold 8.19 km fine-pitch start (~5,600 pages) warms in
	// ~20-45 s -- inside the legs' 90 s preflight, with PrepareRequest's
	// inline fallback keeping every earlier chunk correct.
	double FillBudgetMs()
	{
		static const double Value = []
		{
			double V = 2.0;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuRasterAtlasFillMs="), V);
			return V;
		}();
		return Value;
	}
}

bool FVoxelRasterAtlasCpu::Enabled()
{
	// Command-line latched, never a cvar: -ExecCmds lands after streaming has
	// begun, and an -ExecCmds A/B would silently measure the same
	// configuration twice (the project rule, learned on the GPU fork).
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuRasterAtlas="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

uint32 FVoxelRasterAtlasCpu::PackTag(int64 PageX, int64 PageY)
{
	// Mirrors worldgen.ush's atlasPackTag exactly -- the +32768 bias keeps
	// every reachable page distinct from the 0xffffffff sentinel (which would
	// be page (32767,32767), an order of magnitude past the tile stores'
	// span). The host asserts the range so the mirror cannot silently alias.
	check(PageX >= -32000 && PageX <= 32000 && PageY >= -32000 && PageY <= 32000);
	return (uint32(PageY + 32768) << 16) | (uint32(PageX + 32768) & 0xffffu);
}

uint32 FVoxelRasterAtlasCpu::SlotOf(int64 PageX, int64 PageY) const
{
	// Floor-mod, mirroring atlasTorus.
	const int64 D = int64(PagesDim);
	const int64 Tx = PageX - vxc::floorDiv(PageX, D) * D;
	const int64 Ty = PageY - vxc::floorDiv(PageY, D) * D;
	return uint32(Ty) * PagesDim + uint32(Tx);
}

bool FVoxelRasterAtlasCpu::IsPageResident(int64 PageX, int64 PageY) const
{
	return SlotTags[SlotOf(PageX, PageY)] == PackTag(PageX, PageY);
}

void FVoxelRasterAtlasCpu::Init(int64 InPixelSizeMm, int64 CoverageRadiusMm, int32 MaxCoarseLevel)
{
	check(IsInGameThread());
	check(!IsInitialized());
	check(InPixelSizeMm > 0 && CoverageRadiusMm > 0);
	PixelSizeMm = InPixelSizeMm;
	MaxLevel = MaxCoarseLevel;

	// THE MARGIN, DERIVED THROUGH THE ONE RULE, NEVER RESTATED. For each level
	// the cascade streams, run ComputeRasterWindowPx over the standard chunk
	// dispatch footprint (origin cell -8, 48 columns -- SetChunkFootprint's
	// shape) for chunk (0,0) and measure how far the window reaches past the
	// chunk's own pixels on each side. The widest level wins -- at level 6 the
	// 8-cell halo alone is 51.2 m and the rep-coordinate offset another
	// 3.2 m, on top of the level-independent ~36.5 m cavern reach and 17-px
	// carrier stencil. Deriving this by probe is what keeps the atlas from
	// becoming the second spelling of the window arithmetic that the D5
	// defect (recorded at FillRasterWindow) warns about: if the rule changes,
	// this margin changes with it, in the same commit, automatically.
	int64 MarginPx = 0;
	for (int32 L = 0; L <= MaxLevel; ++L)
	{
		const VoxelGpuRegionBuild::FRasterWindowPx W =
			VoxelGpuRegionBuild::ComputeRasterWindowPx(
				/*OriginVx=*/-8, /*OriginVy=*/-8, FUintVector2(48, 48), L, PixelSizeMm);
		// Chunk (0,0)'s own pixels at level L span [0, chunkSpanPx].
		const int64 ChunkSpanMm = int64(32) * (int64(1) << L) * vxc::kVoxelSizeMm;
		const int64 ChunkSpanPx = vxc::floorDiv(ChunkSpanMm - 1, PixelSizeMm);
		MarginPx = FMath::Max3(MarginPx, -W.PxMin, W.PxMax - ChunkSpanPx);
	}

	const int64 RadiusPx = vxc::floorDiv(CoverageRadiusMm, PixelSizeMm) + 1 + MarginPx;
	RadiusPages = vxc::floorDiv(RadiusPx + int64(kPagePx) - 1, int64(kPagePx)) + 1;
	// +3: one page of recentre hysteresis each side plus the centre page. A
	// desired circle can then never wrap onto itself through the torus, so a
	// resident page is always the page the coverage rule wanted, and slot
	// aliasing only ever replaces pages that left coverage.
	PagesDim = uint32(2 * RadiusPages + 3);

	const uint64 PayloadBytes =
		uint64(PagesDim) * PagesDim * kPagePx * kPagePx * (sizeof(int32) + sizeof(uint32));
	UE_LOG(LogVoxelRasterAtlas, Log,
	       TEXT("[raster-atlas] init: pitch=%lld mm/px, coverage r=%.2f km, margin=%lld px ")
	       TEXT("(probed through ComputeRasterWindowPx over levels 0..%d), torus %ux%u pages of ")
	       TEXT("%u px -> %.1f MiB payload + %.1f KiB tags. Fill budget %.1f ms/tick."),
	       PixelSizeMm, double(CoverageRadiusMm) / 1e6, MarginPx, MaxLevel,
	       PagesDim, PagesDim, kPagePx,
	       double(PayloadBytes) / (1024.0 * 1024.0),
	       double(PagesDim) * PagesDim * sizeof(uint32) / 1024.0,
	       FillBudgetMs());

	SlotTags.Init(0xffffffffu, int32(PagesDim * PagesDim));
	GpuAtlas.Init(kPagePx, PagesDim);
	LastWindowLogSeconds = FPlatformTime::Seconds();
}

void FVoxelRasterAtlasCpu::FillPage(vxc::ITileSampler& Tiles, int64 PageX, int64 PageY)
{
	const double T0 = FPlatformTime::Seconds();
	constexpr uint32 PixelsPerPage = kPagePx * kPagePx;
	const int32 Base = PendingDelta.StagedElevationMm.Num();
	PendingDelta.StagedElevationMm.AddUninitialized(PixelsPerPage);
	PendingDelta.StagedClimatePacked.AddUninitialized(PixelsPerPage);

	// THE SAME FUNNEL FillRasterWindow SAMPLES, same packing, same order --
	// zero new value arithmetic, which is the whole bit-exactness argument: a
	// resident atlas tap returns the identical int32/uint32 the inline window
	// would have carried, so voxel.GPU.VerifyRasterAtlas can demand byte
	// equality rather than tolerance.
	const int64 Px0 = PageX * int64(kPagePx);
	const int64 Py0 = PageY * int64(kPagePx);
	for (uint32 Ly = 0; Ly < kPagePx; ++Ly)
	{
		for (uint32 Lx = 0; Lx < kPagePx; ++Lx)
		{
			const int64 Px = Px0 + Lx;
			const int64 Py = Py0 + Ly;
			const int32 Idx = Base + int32(Lx + Ly * kPagePx);
			PendingDelta.StagedElevationMm[Idx] = Tiles.elevationMm(Px, Py);
			const vxc::ClimateSample Cl = Tiles.climate(Px, Py);
			PendingDelta.StagedClimatePacked[Idx] = uint32(Cl.temperature)
			                                      | (uint32(Cl.seasonality) << 8)
			                                      | (uint32(Cl.precipitation) << 16)
			                                      | (uint32(Cl.precipVariability) << 24);
		}
	}

	const uint32 Slot = SlotOf(PageX, PageY);
	const uint32 Tag = PackTag(PageX, PageY);
	PendingDelta.PageMeta.Add(Slot);
	PendingDelta.PageMeta.Add(Tag);
	PendingDelta.PageMeta.Add(uint32(Base));
	// The mirror flips NOW, one tick-phase ahead of the GPU, and that is
	// sound: this delta flushes at the end of THIS Tick, before any
	// SubmitGpuMeshJob can run and before the job manager's dispatch command
	// is enqueued -- render commands run in enqueue order, so no dispatch
	// that consulted this mirror can reach the GPU ahead of the page.
	SlotTags[Slot] = Tag;

	++PagesFilled;
	FillMicroseconds += uint64((FPlatformTime::Seconds() - T0) * 1e6);
	BytesUploaded += PixelsPerPage * (sizeof(int32) + sizeof(uint32));
}

void FVoxelRasterAtlasCpu::Tick(vxc::ITileSampler& Tiles, int64 AnchorXMm, int64 AnchorYMm)
{
	check(IsInGameThread());
	check(IsInitialized());

	if (!bLoggedPitchMismatch && Tiles.pixelSizeMm() != PixelSizeMm)
	{
		// Pitch is a world property fixed for the session (ActiveTiles'
		// comment); a mismatch here means the atlas was initialized off a
		// different sampler than it is now filling from -- two worlds. Refuse
		// loudly, once, and fill nothing: every request then declines to the
		// inline path, which stays correct.
		UE_LOG(LogVoxelRasterAtlas, Error,
		       TEXT("[raster-atlas] FAIL: sampler pitch %d != atlas pitch %lld -- atlas disabled ")
		       TEXT("for this session; every request will take the inline-window fallback."),
		       Tiles.pixelSizeMm(), PixelSizeMm);
		bLoggedPitchMismatch = true;
	}
	if (bLoggedPitchMismatch)
	{
		LogWindowIfDue();
		return;
	}

	const double Deadline = FPlatformTime::Seconds() + FillBudgetMs() / 1000.0;
	const int64 CenterPageX = vxc::floorDiv(vxc::floorDiv(AnchorXMm, PixelSizeMm), int64(kPagePx));
	const int64 CenterPageY = vxc::floorDiv(vxc::floorDiv(AnchorYMm, PixelSizeMm), int64(kPagePx));

	// Demand pages first: each one is blocking a chunk that already asked.
	int32 DemandIdx = 0;
	for (; DemandIdx < DemandQueue.Num(); ++DemandIdx)
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			break;
		}
		const FPagePt P = DemandQueue[DemandIdx];
		// A demand page can have drifted outside coverage (fast flight away
		// from a declined chunk); filling it anyway is harmless -- it lands
		// in a slot the sweep would only reclaim later -- and simpler than
		// re-deriving whose window it was.
		if (!IsPageResident(P.X, P.Y))
		{
			FillPage(Tiles, P.X, P.Y);
		}
	}
	if (DemandIdx > 0)
	{
		DemandQueue.RemoveAt(0, DemandIdx);
		if (DemandQueue.Num() == 0)
		{
			DemandQueued.Empty();
		}
	}

	// Nearest-first sweep over the coverage circle: expanding Chebyshev rings
	// from the anchor's page, so the ground under and just ahead of the
	// player is always the freshest. The scan is mirror lookups only
	// (~(2R+1)^2 = a few thousand array reads) until it finds work.
	for (int64 R = 0; R <= RadiusPages; ++R)
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			break;
		}
		const int64 X0 = CenterPageX - R;
		const int64 X1 = CenterPageX + R;
		const int64 Y0 = CenterPageY - R;
		const int64 Y1 = CenterPageY + R;
		for (int64 Py = Y0; Py <= Y1 && FPlatformTime::Seconds() < Deadline; ++Py)
		{
			// Ring perimeter only: interior rings were covered at smaller R.
			const int64 StepX = (Py == Y0 || Py == Y1) ? 1 : (X1 - X0 > 0 ? X1 - X0 : 1);
			for (int64 Px = X0; Px <= X1; Px += StepX)
			{
				if (!IsPageResident(Px, Py))
				{
					FillPage(Tiles, Px, Py);
					if (FPlatformTime::Seconds() >= Deadline)
					{
						break;
					}
				}
			}
		}
	}

	// Flush: one render command, enqueued BEFORE any dispatch this tick can
	// enqueue -- the ordering half of the mirror's soundness argument.
	if (PendingDelta.PageMeta.Num() > 0 || PendingDelta.InvalidateSlots.Num() > 0 ||
	    PendingDelta.bClearMissStats)
	{
		GpuAtlas.EnqueueUpsert(MoveTemp(PendingDelta));
		PendingDelta = FVoxelRasterAtlasGpuDelta();
	}

	LogWindowIfDue();
}

void FVoxelRasterAtlasCpu::LogWindowIfDue()
{
	const double Now = FPlatformTime::Seconds();
	if (Now - LastWindowLogSeconds < kWindowSeconds)
	{
		return;
	}
	LastWindowLogSeconds = Now;

	// Collect last window's GPU miss stats (armed a window ago), then arm the
	// read-and-clear for the next window. The copy runs BEFORE the clear in
	// the upsert graph, so a window never reads its own clear.
	FVoxelRasterAtlasMissStats Miss;
	const bool bHaveMiss = GpuAtlas.PollMissStats(Miss);
	GpuAtlas.EnqueueMissStatsRead();
	PendingDelta.bClearMissStats = true;

	uint32 ResidentPages = 0;
	for (const uint32 Tag : SlotTags)
	{
		ResidentPages += (Tag != 0xffffffffu) ? 1u : 0u;
	}

	if (bHaveMiss && Miss.Misses > 0)
	{
		GpuMissLifetime += Miss.Misses;
		// THE FAILING READING. A GPU tap reached a page the coverage check
		// said was resident -- the layered design's "impossible" case -- and
		// the affected chunks were generated over the missing-tile answer:
		// sea-level flat terrain at the miss site, silently different from
		// the CPU reference. Every chunk dispatched this window is suspect.
		FString Pages;
		for (int32 I = 0; I < 4; ++I)
		{
			if (Miss.FirstPageTags[I] != 0)
			{
				Pages += FString::Printf(TEXT(" (%d,%d)"),
					int32(Miss.FirstPageTags[I] & 0xffffu) - 32768,
					int32(Miss.FirstPageTags[I] >> 16) - 32768);
			}
		}
		UE_LOG(LogVoxelRasterAtlas, Error,
		       TEXT("[raster-atlas] window: gpuMiss=%u FAIL firstPages=%s -- atlas-armed chunks ")
		       TEXT("this window sampled missing pages and generated WRONG (sea-level) terrain; ")
		       TEXT("treat the window's output as suspect and this leg as invalid."),
		       Miss.Misses, *Pages);
	}

	// The healthy line. Reading rules (also in the header): served=0 with the
	// fork dispatching is the DEAD reading; inlineFallback not falling toward
	// zero after warmup means prefetch is broken and the leg is measuring the
	// control path wearing the atlas flag.
	UE_LOG(LogVoxelRasterAtlas, Log,
	       TEXT("[raster-atlas] window: served=%llu inlineFallback=%llu fills=%llu ")
	       TEXT("(%.2f MiB, %.1f ms GT) resident=%u/%u pages gpuMiss=%s lifetimeMiss=%llu"),
	       Served, DeclinedCold, PagesFilled,
	       double(BytesUploaded) / (1024.0 * 1024.0),
	       double(FillMicroseconds) / 1000.0,
	       ResidentPages, PagesDim * PagesDim,
	       bHaveMiss ? *FString::Printf(TEXT("%u"), Miss.Misses) : TEXT("PENDING"),
	       GpuMissLifetime);

	Served = 0;
	DeclinedCold = 0;
	PagesFilled = 0;
	FillMicroseconds = 0;
	BytesUploaded = 0;
}

bool FVoxelRasterAtlasCpu::PrepareRequest(FVoxelGpuRegionRequest& Req)
{
	check(IsInGameThread());
	check(IsInitialized());
	if (bLoggedPitchMismatch)
	{
		return false;
	}

	// THE COVERAGE CHECK IS THE WINDOW RULE -- the same call FillRasterWindow
	// makes, over the same request fields, at the same pitch. Undersizing is
	// therefore impossible to introduce HERE without also breaking the inline
	// path and both verify harnesses, which is the structural answer to "do
	// not become the second spelling".
	const VoxelGpuRegionBuild::FRasterWindowPx W =
		VoxelGpuRegionBuild::ComputeRasterWindowPx(
			Req.OriginVx, Req.OriginVy, Req.DispatchColumns, Req.CoarseLevel, PixelSizeMm);

	const int64 Page0X = vxc::floorDiv(W.PxMin, int64(kPagePx));
	const int64 Page1X = vxc::floorDiv(W.PxMax, int64(kPagePx));
	const int64 Page0Y = vxc::floorDiv(W.PyMin, int64(kPagePx));
	const int64 Page1Y = vxc::floorDiv(W.PyMax, int64(kPagePx));

	bool bCovered = true;
	for (int64 Py = Page0Y; Py <= Page1Y; ++Py)
	{
		for (int64 Px = Page0X; Px <= Page1X; ++Px)
		{
			if (IsPageResident(Px, Py))
			{
				continue;
			}
			bCovered = false;
			// Demand-queue the hole so next tick fills it first; the chunk
			// itself is NOT delayed -- the caller falls back to the inline
			// window and dispatches this tick.
			const uint64 Key = (uint64(uint32(int32(Py))) << 32) | uint64(uint32(int32(Px)));
			if (!DemandQueued.Contains(Key))
			{
				DemandQueued.Add(Key);
				DemandQueue.Add(FPagePt{Px, Py});
			}
		}
	}

	if (!bCovered)
	{
		++DeclinedCold;
		return false;
	}

	Req.bRasterAtlas = true;
	Req.RasterAtlas = &GpuAtlas;
	Req.PixelSizeMm = int32(PixelSizeMm);
	// The atlas form carries no window -- ValidateRegionRequest refuses
	// half-and-half, so make the "no window" half true explicitly.
	Req.RasterOriginPx = FIntPoint::ZeroValue;
	Req.RasterSize = FUintVector2(0, 0);
	Req.ElevationMm.Reset();
	Req.ClimatePacked.Reset();
	++Served;
	return true;
}

void FVoxelRasterAtlasCpu::FillWindowNow(vxc::ITileSampler& Tiles, const FVoxelGpuRegionRequest& Req)
{
	check(IsInGameThread());
	check(IsInitialized());
	// The SAME rule PrepareRequest and FillRasterWindow run -- gate coverage
	// is derived, never respelled.
	const VoxelGpuRegionBuild::FRasterWindowPx W =
		VoxelGpuRegionBuild::ComputeRasterWindowPx(
			Req.OriginVx, Req.OriginVy, Req.DispatchColumns, Req.CoarseLevel, PixelSizeMm);
	for (int64 Py = vxc::floorDiv(W.PyMin, int64(kPagePx));
	     Py <= vxc::floorDiv(W.PyMax, int64(kPagePx)); ++Py)
	{
		for (int64 Px = vxc::floorDiv(W.PxMin, int64(kPagePx));
		     Px <= vxc::floorDiv(W.PxMax, int64(kPagePx)); ++Px)
		{
			if (!IsPageResident(Px, Py))
			{
				FillPage(Tiles, Px, Py);
			}
		}
	}
	if (PendingDelta.PageMeta.Num() > 0)
	{
		GpuAtlas.EnqueueUpsert(MoveTemp(PendingDelta));
		PendingDelta = FVoxelRasterAtlasGpuDelta();
	}
}

void FVoxelRasterAtlasCpu::DebugDropPage(int64 PageX, int64 PageY)
{
	check(IsInGameThread());
	check(IsInitialized());
	const uint32 Slot = SlotOf(PageX, PageY);
	SlotTags[Slot] = 0xffffffffu;
	FVoxelRasterAtlasGpuDelta Drop;
	Drop.InvalidateSlots.Add(Slot);
	GpuAtlas.EnqueueUpsert(MoveTemp(Drop));
}

void FVoxelRasterAtlasCpu::Shutdown()
{
	check(IsInGameThread());
	if (!IsInitialized())
	{
		return;
	}
	FVoxelRasterAtlasGpu* Gpu = &GpuAtlas;
	ENQUEUE_RENDER_COMMAND(VoxelRasterAtlasRelease)(
		[Gpu](FRHICommandListImmediate&)
	{
		Gpu->ReleaseResources_RenderThread();
	});
}
