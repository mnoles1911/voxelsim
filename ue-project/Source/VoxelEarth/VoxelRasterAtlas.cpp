// FVoxelRasterAtlasCpu -- policy side of the persistent GPU raster atlas.
// See the header for the design, the fill-mode ladder and the failing
// readings; this file is the mechanics: margin derivation THROUGH the one
// window rule, nearest-first fills under a time budget, the coverage check,
// the per-page cost breakdown, and the stats window.

#include "VoxelRasterAtlas.h"

#include "VoxelGpuWorldGen.h"

#include "voxelcore/core.h"

#include "Misc/CommandLine.h"
#include "RenderingThread.h"   // ENQUEUE_RENDER_COMMAND (Shutdown)
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"   // ON_SCOPE_EXIT -- PrepareRequest has several returns

DEFINE_LOG_CATEGORY_STATIC(LogVoxelRasterAtlas, Log, All);

namespace
{
	// Stats cadence.
	//
	// THIS USED TO BE A HARDCODED 5.0 AND THAT WAS THE THIRD INSTANCE OF THE
	// SAME DEFECT THIS PROJECT HAS NOW FOUND IN THREE DIFFERENT FILES. The old
	// comment claimed the 5 s cadence made the atlas line "land beside the
	// lines it will be read against" -- and every flight leg passes
	// -VoxelPerfLogInterval=2, so it did the opposite: this line's windows were
	// 2.5x wider than `Voxel tick budget`'s and `[gpu-resid]`'s, and any
	// side-by-side reading across them was wrong by that factor. (See the two
	// CORRECTION sections in docs/50k-budget-2026-08-23.md: MaybeLogCounters'
	// hardcoded WindowMs, and MaybeLog's hardcoded 5.0 in the residency path.)
	//
	// Now read from the SAME switch the producers use, and the real elapsed
	// seconds are printed as `win=%.2fs` at the END of the line -- the
	// old-leg-grep rule, so a grep written against the old format still lands.
	double WindowSeconds()
	{
		static const double Value = []
		{
			float V = 5.0f;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfLogInterval="), V);
			return double(FMath::Max(0.25f, V));
		}();
		return Value;
	}

	// Per-tick fill budget in milliseconds of game-thread sampler time.
	//
	// NOTE THE OVERSHOOT, because it is why the measured cost is over twice the
	// budget: the deadline is tested BEFORE a page is filled, never during, so
	// a tick that reaches the test with 0.1 ms spent still pays a whole
	// 2.83 ms page. At 2.0 ms/tick and 2.83 ms/page the steady state is
	// EXACTLY ONE PAGE PER TICK, ~2.83 ms, and the leg's own numbers say so:
	// 553-1,226 ms per 5 s window against the 500 ms the budget nominally
	// allows. Raising the budget does not change the shape and lowering it
	// below one page does not either -- which is the argument for making the
	// page cheaper (mode 2) or moving it (mode 3) rather than tuning this.
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

	// THE COARSE CLIMATE PITCH, in mm, and why it is a switch rather than
	// derived here.
	//
	// The atlas fills through vxc::ITileSampler, which exposes ONE pitch: its
	// own (1,875 mm at the fine tier). The fine tier carries no climate plane,
	// so FineTileSampler::climate delegates to the coarse sampler and does the
	// conversion itself with floorDiv(px * finePitch, coarsePitch) -- and its
	// own comment warns, in as many words, against anyone restating that as a
	// hardcoded /16. So this file does not restate it: it is TOLD the coarse
	// pitch, and if nobody tells it, the dedup does not run at all.
	//
	// Default 0 = NOT TOLD. That is a distinct state from "told, and it saves
	// nothing", and the window line prints `climateDedup=UNAVAILABLE` for it so
	// a leg that forgot the switch cannot be read as a leg where the dedup
	// bought nothing. The tile-grid line already prints the number to pass:
	//   Voxel tile grid: dir=... loaded=289 ... scale=1
	// and scale 1 is vxc::tilePixelSizeMm(1) = 30000.
	//
	// A WRONG VALUE HERE WRITES WRONG CLIMATE INTO THE ATLAS, silently, which
	// is why the audit below is on by default and why a mismatch disables the
	// dedup for the session rather than logging and continuing. Passing a wrong
	// pitch (e.g. =60000) is also the intended way to watch that gate go RED.
	int32 ClimatePitchMm()
	{
		static const int32 Value = []
		{
			int32 V = 0;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuRasterAtlasClimatePitchMm="), V);
			return V;
		}();
		return Value;
	}

	// Climate-dedup audit probes per page. Each one re-samples climate at a
	// deterministic pseudo-random pixel of the page and compares against what
	// the dedup wrote there.
	//
	// 64 by default: that is 64 extra sampler calls against the 64 the dedup
	// left, i.e. the audited fill still makes 128 calls where the undeduped
	// one makes 16,384 -- 128x fewer, with the check running on every page of
	// every leg rather than in a gate somebody has to remember to run.
	//
	// WHAT IT CAN AND CANNOT CATCH, stated rather than implied. A wrong run
	// length disagrees with the truth on roughly half the pixels of a page
	// wherever climate actually varies, so 64 probes catch it on the first page
	// with overwhelming probability. It CANNOT catch a wrong run length over
	// ground whose climate is genuinely uniform -- there the deduped value is
	// also the right value, so nothing is wrong yet, and the next varied page
	// catches it. `checked` is printed beside `mismatch` so `checked=0` (the
	// audit never ran) can never be read as `mismatch=0` (it ran and passed).
	int32 ClimateAuditProbes()
	{
		static const int32 Value = []
		{
			int32 V = 64;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuRasterAtlasClimateAudit="), V);
			return FMath::Clamp(V, 0, int32(FVoxelRasterAtlasCpu::kPagePx * FVoxelRasterAtlasCpu::kPagePx));
		}();
		return Value;
	}

	// --- mode 3: the off-game-thread fill ------------------------------------
	//
	// THE HAZARD, NAMED BEFORE THE KNOB. FVoxelFineTileSamplerProxy::elevationMm
	// has two cold paths and they are NOT symmetric:
	//   * on the GAME THREAD a non-resident pixel takes Lock_ exclusively and
	//     BLOCKS on a synchronous tile load -- slow, but correct, and it is
	//     what the atlas fill does today.
	//   * on a WORKER the same pixel takes Lock_ exclusively and goes straight
	//     to ReportGateLeak_Locked, which is **FATAL on an unattended run** by
	//     deliberate design (VoxelFineTileStreamer.cpp: "an unattended run that
	//     continues past this point does not degrade, it LIES").
	// So naively moving this fill to a worker converts a silent blocking load
	// into a killed leg. The guard is in LaunchAsyncSlots: every page is PRIMED
	// on the game thread with four corner samples first -- a page is 128 px and
	// a fine tile is 8,192, so four corners name exactly the tiles the page can
	// span -- which forces the game-thread branch and leaves the worker reading
	// resident tiles only. Four calls per page against 32,768 is 0.01%.
	//
	// The residual risk, stated: TickResidencyAndEviction could evict a tile
	// between the prime and the worker's read. With ringRadius=0 and 1.82 of a
	// 12.00 GiB budget resident, no eviction happens on a cold start -- but if
	// one ever did, the reading is `gateLeaks>0` on the `Fine tier` line (or a
	// fatal), and mode 3 must not ship on that evidence.
	int32 AsyncFillTasks()
	{
		static const int32 Value = []
		{
			int32 V = 3;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuRasterAtlasFillTasks="), V);
			return FMath::Clamp(V, 1, 8);
		}();
		return Value;
	}

	// Pages per launched batch. 8 x 2.83 ms = ~23 ms of worker per batch, which
	// is fine enough that residency arrives in small steps (a page is only
	// useful once its whole batch lands) and coarse enough that the launch
	// overhead is invisible. With mode 2's dedup a batch is ~14 ms.
	int32 AsyncFillBatchPages()
	{
		static const int32 Value = []
		{
			int32 V = 8;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuRasterAtlasFillBatch="), V);
			return FMath::Clamp(V, 1, 64);
		}();
		return Value;
	}

	const TCHAR* FillModeName(int32 Mode)
	{
		switch (Mode)
		{
		case 0:  return TEXT("0 square sweep, per-pixel, game thread (control)");
		case 1:  return TEXT("1 coverage DISC");
		case 2:  return TEXT("2 disc + climate dedup");
		default: return TEXT("3 disc + dedup + OFF the game thread");
		}
	}

	double MsFromCycles(uint64 Cycles)
	{
		return double(Cycles) * FPlatformTime::GetSecondsPerCycle64() * 1000.0;
	}

	// Deterministic probe positions for the climate audit: a page's probes are
	// a function of its coordinates alone, so a mismatch is reproducible from
	// the log line that reports it rather than being a race nobody can re-hit.
	uint32 ProbeHash(int64 PageX, int64 PageY, int32 I)
	{
		uint64 H = 1469598103934665603ull;
		const auto Mix = [&H](uint64 V)
		{
			H ^= V;
			H *= 1099511628211ull;
		};
		Mix(uint64(PageX) * 0x9E3779B97F4A7C15ull);
		Mix(uint64(PageY) * 0xC2B2AE3D27D4EB4Full);
		Mix(uint64(uint32(I)) * 0x165667B19E3779F9ull);
		return uint32(H ^ (H >> 32));
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

int32 FVoxelRasterAtlasCpu::FillMode()
{
	static const int32 Value = []
	{
		int32 V = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuRasterAtlasFill="), V);
		return FMath::Clamp(V, 0, 3);
	}();
	return Value;
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

uint64 FVoxelRasterAtlasCpu::PageKey(int64 PageX, int64 PageY)
{
	// ONE spelling, used by the demand set and the in-flight set alike. It was
	// written out inline in PrepareRequest and would have been written out a
	// second time for the in-flight set -- two spellings of an identity is how
	// a page gets queued twice or, worse, gets treated as in flight when it is
	// not. The range check is PackTag's, which every reachable page already
	// passes.
	check(PageX >= -32000 && PageX <= 32000 && PageY >= -32000 && PageY <= 32000);
	return (uint64(uint32(int32(PageY))) << 32) | uint64(uint32(int32(PageX)));
}

uint32 FVoxelRasterAtlasCpu::PackClimate(const vxc::ClimateSample& C)
{
	return uint32(C.temperature)
	     | (uint32(C.seasonality) << 8)
	     | (uint32(C.precipitation) << 16)
	     | (uint32(C.precipVariability) << 24);
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

bool FVoxelRasterAtlasCpu::IsPageInCoverage(int64 PageX, int64 PageY,
                                            int64 AnchorPxX, int64 AnchorPxY) const
{
	// THE SWEEP VISITS A SQUARE; COVERAGE IS A DISC. RadiusPages is a Chebyshev
	// radius rounded up from CoverageRadiusPx, so the 39x39 square the sweep
	// walks circumscribes the circle the rings actually admit to (AdmitOuterUU
	// is a RADIUS -- VoxelWorldSubsystem.cpp). At the shipped geometry
	// (CoverageRadiusPx=2,237 px = 4.19 km, RadiusPages=19) the disc admits
	// 1,021-1,036 of the square's 1,521 pages depending on where inside its own
	// page the anchor sits -- the corners are ~32% of the sweep, and the square
	// reaches 6.45 km at its corner against a 4.19 km circle, so no chunk in
	// the cascade ever asks for them.
	//
	// The test is the NEAREST pixel of the page against CoverageRadiusPx, which
	// over-covers on purpose: a page that only clips the circle is filled. And
	// it cannot generate wrong terrain even if it were too tight -- a page the
	// sweep declines to prefetch is still demand-queued and inline-filled by
	// PrepareRequest, so the cost of being wrong here is `inlineFallback`, not
	// silence.
	const int64 X0 = PageX * int64(kPagePx);
	const int64 X1 = X0 + int64(kPagePx) - 1;
	const int64 Y0 = PageY * int64(kPagePx);
	const int64 Y1 = Y0 + int64(kPagePx) - 1;
	const int64 Dx = (AnchorPxX < X0) ? (X0 - AnchorPxX) : ((AnchorPxX > X1) ? (AnchorPxX - X1) : 0);
	const int64 Dy = (AnchorPxY < Y0) ? (Y0 - AnchorPxY) : ((AnchorPxY > Y1) ? (AnchorPxY - Y1) : 0);
	return Dx * Dx + Dy * Dy <= CoverageRadiusPx * CoverageRadiusPx;
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
	// KEPT, not recomputed: mode 1's disc test is this same radius, so the
	// prefetch circle and the margin probe can never drift apart.
	CoverageRadiusPx = RadiusPx;
	RadiusPages = vxc::floorDiv(RadiusPx + int64(kPagePx) - 1, int64(kPagePx)) + 1;
	// +3: one page of recentre hysteresis each side plus the centre page. A
	// desired circle can then never wrap onto itself through the torus, so a
	// resident page is always the page the coverage rule wanted, and slot
	// aliasing only ever replaces pages that left coverage.
	PagesDim = uint32(2 * RadiusPages + 3);

	// THE CLIMATE RUN LENGTH, from the pitch the host was asked to supply.
	// Refused rather than rounded when the coarse pitch is not a whole multiple
	// of the fine one: FineTileSampler::climate routes with
	// floorDiv(px * finePitch, coarsePitch), which is only a clean
	// floorDiv(px, K) when coarsePitch = K * finePitch. A non-multiple pitch
	// would make runs of unequal length and the dedup would be wrong at some
	// boundaries -- so it does not run at all, loudly.
	const int32 ClimatePitch = ClimatePitchMm();
	if (ClimatePitch > 0)
	{
		if (ClimatePitch < PixelSizeMm || (int64(ClimatePitch) % PixelSizeMm) != 0)
		{
			UE_LOG(LogVoxelRasterAtlas, Error,
			       TEXT("[raster-atlas] climate dedup REFUSED: -VoxelGpuRasterAtlasClimatePitchMm=%d is not a ")
			       TEXT("whole multiple of the atlas pitch %lld mm/px. FineTileSampler::climate routes with ")
			       TEXT("floorDiv(px*fine, coarse), which is a clean run only when it is. Dedup OFF; fills ")
			       TEXT("stay per-pixel."),
			       ClimatePitch, PixelSizeMm);
		}
		else
		{
			ClimateRunPx = int64(ClimatePitch) / PixelSizeMm;
		}
	}

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

	// THE ONE LINE THAT SAYS WHICH LADDER RUNG THIS RUN IS ON. Without it a
	// leg's log cannot be told apart from the control's, which is how an arm
	// gets read as a result of the thing it was not running.
	UE_LOG(LogVoxelRasterAtlas, Log,
	       TEXT("[raster-atlas] fill mode=%s | coverageRadius=%lld px, sweep square %lldx%lld pages ")
	       TEXT("| climateDedup=%s (pitch=%d mm, run=%lld px, audit=%d probes/page) ")
	       TEXT("| async tasks=%d batch=%d pages"),
	       FillModeName(FillMode()), CoverageRadiusPx, 2 * RadiusPages + 1, 2 * RadiusPages + 1,
	       (FillMode() >= 2)
	           ? (ClimateRunPx > 1 ? TEXT("ON") : TEXT("UNAVAILABLE -- pass -VoxelGpuRasterAtlasClimatePitchMm="))
	           : TEXT("off (mode < 2)"),
	       ClimatePitch, ClimateRunPx, ClimateAuditProbes(),
	       AsyncFillTasks(), AsyncFillBatchPages());

	SlotTags.Init(0xffffffffu, int32(PagesDim * PagesDim));
	GpuAtlas.Init(kPagePx, PagesDim);
	LastWindowLogSeconds = FPlatformTime::Seconds();
}

// ---------------------------------------------------------------------------
// SAMPLING -- the three phases the window line's buckets name.
//
// SPLIT INTO PHASES ON PURPOSE, and the reason is the whole of task one: with
// one interleaved loop the 2.83 ms/page is a single number nobody can act on.
// Three phases with one Cycles64 pair each cost six timer reads per page
// against 32,768 sampler calls -- 0.005% -- and turn "2.83 ms" into
// "elev X, climate Y, stage Z, resid R", which is what decides whether the fix
// is a cheaper sampler call, fewer calls, or a different thread.
//
// The phase split does NOT change a single value: elevationMm and climate are
// pure functions of (px, py), so sampling all the elevations and then all the
// climates writes the same two planes the interleaved loop wrote. Mode 0 is
// therefore value-identical to the shipped path, not byte-identical to its
// instruction stream -- stated rather than glossed, because "control" has to
// mean something exact.
// ---------------------------------------------------------------------------

void FVoxelRasterAtlasCpu::SamplePage(vxc::ITileSampler& Tiles, int64 PageX, int64 PageY,
                                      int32* OutElevation, uint32* OutClimate,
                                      FPageFillCost& Cost) const
{
	const uint64 TotalT0 = FPlatformTime::Cycles64();
	const int64 Px0 = PageX * int64(kPagePx);
	const int64 Py0 = PageY * int64(kPagePx);

	// --- phase 1: elevation, one sampler call per pixel ---------------------
	// IRREDUCIBLE IN CALL COUNT through this interface. Every atlas pixel is a
	// distinct control point, so all 16,384 values are genuinely different --
	// there is no dedup here, only a cheaper way to ask, and vxc::ITileSampler
	// has no bulk form to ask it with. See the report's hook 3.
	{
		const uint64 T0 = FPlatformTime::Cycles64();
		for (uint32 Ly = 0; Ly < kPagePx; ++Ly)
		{
			const int64 Py = Py0 + Ly;
			int32* Row = OutElevation + Ly * kPagePx;
			for (uint32 Lx = 0; Lx < kPagePx; ++Lx)
			{
				Row[Lx] = Tiles.elevationMm(Px0 + Lx, Py);
			}
		}
		Cost.ElevCalls += uint64(kPagePx) * kPagePx;
		Cost.ElevCycles += FPlatformTime::Cycles64() - T0;
	}

	// --- phase 2: climate ----------------------------------------------------
	// THE 256x REDUNDANCY. The fine tier carries no climate plane, so every one
	// of these lands in the COARSE sampler through FineTileSampler::climate's
	// floorDiv(px * 1875, 30000) = floorDiv(px, 16). One coarse cell is
	// 16x16 = 256 atlas pixels; a 128-px page needs 8x8 = 64 distinct samples
	// and mode 0 makes 16,384 calls to fetch them. The dedup is two memos, one
	// per axis: a row whose coarse row index repeats is a memcpy of the row
	// before it, and within a fresh row a repeated coarse column index reuses
	// the value in hand.
	const bool bDedup = (FillMode() >= 2) && ClimateRunPx > 1 &&
	                    !bClimateDedupDisabled.load(std::memory_order_relaxed);
	{
		const uint64 T0 = FPlatformTime::Cycles64();
		if (!bDedup)
		{
			for (uint32 Ly = 0; Ly < kPagePx; ++Ly)
			{
				const int64 Py = Py0 + Ly;
				uint32* Row = OutClimate + Ly * kPagePx;
				for (uint32 Lx = 0; Lx < kPagePx; ++Lx)
				{
					Row[Lx] = PackClimate(Tiles.climate(Px0 + Lx, Py));
				}
			}
			Cost.ClimateCalls += uint64(kPagePx) * kPagePx;
		}
		else
		{
			const int64 K = ClimateRunPx;
			int64 LastCellY = MIN_int64;
			const uint32* PrevRow = nullptr;
			for (uint32 Ly = 0; Ly < kPagePx; ++Ly)
			{
				const int64 Py = Py0 + Ly;
				uint32* Row = OutClimate + Ly * kPagePx;
				const int64 CellY = vxc::floorDiv(Py, K);
				if (CellY == LastCellY && PrevRow != nullptr)
				{
					FMemory::Memcpy(Row, PrevRow, kPagePx * sizeof(uint32));
					continue;
				}
				int64 LastCellX = MIN_int64;
				uint32 Held = 0;
				for (uint32 Lx = 0; Lx < kPagePx; ++Lx)
				{
					const int64 Px = Px0 + Lx;
					const int64 CellX = vxc::floorDiv(Px, K);
					if (CellX != LastCellX)
					{
						LastCellX = CellX;
						Held = PackClimate(Tiles.climate(Px, Py));
						++Cost.ClimateCalls;
					}
					Row[Lx] = Held;
				}
				LastCellY = CellY;
				PrevRow = Row;
			}

			// --- the audit ---------------------------------------------------
			// THE GATE THAT MUST BE ABLE TO GO RED. Re-sample climate at
			// deterministic pseudo-random pixels of this page and compare
			// against what the dedup wrote. A wrong run length (pass
			// -VoxelGpuRasterAtlasClimatePitchMm=60000 to watch it) disagrees
			// on about half the pixels of any page whose climate varies, so
			// this lights up on the first such page. The counters are carried
			// out on Cost; the GAME THREAD is what reacts, because disabling
			// the dedup and logging must not happen from three workers at once.
			const int32 Probes = ClimateAuditProbes();
			for (int32 I = 0; I < Probes; ++I)
			{
				const uint32 H = ProbeHash(PageX, PageY, I);
				const uint32 Lx = H % kPagePx;
				const uint32 Ly = (H / kPagePx) % kPagePx;
				const uint32 Truth = PackClimate(Tiles.climate(Px0 + Lx, Py0 + Ly));
				++Cost.AuditChecked;
				if (Truth != OutClimate[Ly * kPagePx + Lx])
				{
					++Cost.AuditMismatch;
				}
			}
		}
		Cost.ClimatePixels += uint64(kPagePx) * kPagePx;
		// THE AUDIT'S OWN CALLS ARE IN THIS TIME AND NOT IN ClimateCalls, on
		// purpose: the bucket is "what the climate plane cost", and the audit
		// is part of what it cost. So do NOT divide climate ms by
		// callsPerPage.climate to get a per-call figure with the dedup on --
		// the divisor is climate + audit, both of which the line prints.
		Cost.ClimateCycles += FPlatformTime::Cycles64() - T0;
	}

	++Cost.Pages;
	Cost.TotalCycles += FPlatformTime::Cycles64() - TotalT0;
}

// ---------------------------------------------------------------------------

int32 FVoxelRasterAtlasCpu::StagePageStorage()
{
	constexpr uint32 PixelsPerPage = kPagePx * kPagePx;
	const int32 Base = PendingDelta.StagedElevationMm.Num();
	PendingDelta.StagedElevationMm.AddUninitialized(PixelsPerPage);
	PendingDelta.StagedClimatePacked.AddUninitialized(PixelsPerPage);
	return Base;
}

void FVoxelRasterAtlasCpu::CommitStagedPage(int64 PageX, int64 PageY, int32 Base)
{
	constexpr uint32 PixelsPerPage = kPagePx * kPagePx;
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
	//
	// UNCHANGED BY MODE 3, and this is the load-bearing half of the async
	// design: a worker only ever writes into ITS OWN slot buffers. Nothing is
	// staged and no tag is flipped until the game thread harvests a FINISHED
	// slot, here, in the same tick the delta flushes. A page in flight reads as
	// NOT resident, so PrepareRequest declines it and the chunk takes the
	// inline path -- correct, just not yet cheap.
	SlotTags[Slot] = Tag;

	++PagesFilled;
	++PagesFilledLifetime;
	BytesUploaded += PixelsPerPage * (sizeof(int32) + sizeof(uint32));
}

void FVoxelRasterAtlasCpu::FillPage(vxc::ITileSampler& Tiles, int64 PageX, int64 PageY)
{
	const uint64 StageT0 = FPlatformTime::Cycles64();
	const int32 Base = StagePageStorage();
	const uint64 StageCycles = FPlatformTime::Cycles64() - StageT0;

	// Sampled STRAIGHT INTO the delta -- no scratch, no copy. The two arrays
	// cannot reallocate between here and the end of SamplePage because nothing
	// else appends to them in that span.
	SamplePage(Tiles, PageX, PageY,
	           PendingDelta.StagedElevationMm.GetData() + Base,
	           PendingDelta.StagedClimatePacked.GetData() + Base,
	           WindowCost);
	WindowCost.StageCycles += StageCycles;
	WindowCost.TotalCycles += StageCycles;

	CommitStagedPage(PageX, PageY, Base);
}

// ---------------------------------------------------------------------------
// THE ONE FILL-ORDER POLICY. Both the synchronous path and the asynchronous
// one call this, so the A/B's two arms differ in WHERE the sampling runs and
// in nothing else. If they each had their own sweep, a settle difference could
// be an ordering difference and nobody could tell.
// ---------------------------------------------------------------------------

bool FVoxelRasterAtlasCpu::NextPageToFill(int64 AnchorPxX, int64 AnchorPxY,
                                          FPagePt& Out, bool& bOutFromDemand)
{
	bOutFromDemand = false;

	// Demand pages first: each one is blocking a chunk that already asked.
	while (DemandQueue.Num() > 0)
	{
		const FPagePt P = DemandQueue[0];
		// A demand page can have drifted outside coverage (fast flight away
		// from a declined chunk); filling it anyway is harmless -- it lands in
		// a slot the sweep would only reclaim later -- and simpler than
		// re-deriving whose window it was. It can also have become resident, or
		// been handed to a worker, since it was queued: drop those.
		if (IsPageResident(P.X, P.Y) || PagesInFlight.Contains(PageKey(P.X, P.Y)))
		{
			DemandQueue.RemoveAt(0);
			if (DemandQueue.Num() == 0)
			{
				DemandQueued.Empty();
			}
			continue;
		}
		Out = P;
		bOutFromDemand = true;
		return true;
	}

	// Nearest-first sweep: expanding Chebyshev rings from the anchor's page, so
	// the ground under and just ahead of the player is always the freshest. The
	// scan is mirror lookups only (~(2R+1)^2 = a few thousand array reads)
	// until it finds work.
	const bool bDisc = FillMode() >= 1;
	const int64 CenterPageX = vxc::floorDiv(AnchorPxX, int64(kPagePx));
	const int64 CenterPageY = vxc::floorDiv(AnchorPxY, int64(kPagePx));
	// The cursor is only valid for the centre it was built against: when the
	// anchor crosses a page boundary the whole ring numbering changes.
	if (CenterPageX != SweepCenterPageX || CenterPageY != SweepCenterPageY)
	{
		SweepCenterPageX = CenterPageX;
		SweepCenterPageY = CenterPageY;
		SweepResumeR = 0;
	}

	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		// Pass 0 resumes at the last ring that had work; pass 1 (only if that
		// found nothing, and only if it started above zero) rescans from the
		// centre, so a hole opened behind the cursor -- an invalidated page, a
		// demand page that aliased a slot -- is always eventually found.
		const int64 StartR = (Attempt == 0) ? SweepResumeR : 0;
		if (Attempt == 1 && StartR == SweepResumeR)
		{
			break; // pass 0 already started at 0; a second identical scan is dead work
		}
		for (int64 R = StartR; R <= RadiusPages; ++R)
		{
			const int64 X0 = CenterPageX - R;
			const int64 X1 = CenterPageX + R;
			const int64 Y0 = CenterPageY - R;
			const int64 Y1 = CenterPageY + R;
			for (int64 Py = Y0; Py <= Y1; ++Py)
			{
				// Ring perimeter only: interior rings were covered at smaller R.
				const int64 StepX = (Py == Y0 || Py == Y1) ? 1 : (X1 - X0 > 0 ? X1 - X0 : 1);
				for (int64 Px = X0; Px <= X1; Px += StepX)
				{
					if (IsPageResident(Px, Py) || PagesInFlight.Contains(PageKey(Px, Py)))
					{
						continue;
					}
					if (bDisc && !IsPageInCoverage(Px, Py, AnchorPxX, AnchorPxY))
					{
						continue;
					}
					SweepResumeR = R;
					Out = FPagePt{Px, Py};
					return true;
				}
			}
		}
	}
	return false;
}

void FVoxelRasterAtlasCpu::PopDemandFront()
{
	DemandQueue.RemoveAt(0);
	if (DemandQueue.Num() == 0)
	{
		DemandQueued.Empty();
	}
}

// ---------------------------------------------------------------------------
// MODE 3 -- the off-game-thread fill. See AsyncFillTasks()'s comment for the
// fatal-gate-leak hazard this is written around; the prime probe below is the
// guard.
// ---------------------------------------------------------------------------

void FVoxelRasterAtlasCpu::HarvestAsyncSlots()
{
	constexpr uint32 PixelsPerPage = kPagePx * kPagePx;
	for (TUniquePtr<FFillSlot>& SlotPtr : FillSlots)
	{
		FFillSlot* S = SlotPtr.Get();
		if (S == nullptr || !S->bBusy)
		{
			continue;
		}
		// ACQUIRE against the worker's RELEASE. Everything the worker wrote
		// into Elev/Climate/Cost happens-before this load returning true; that
		// single pair is the whole synchronisation, because nothing else in the
		// slot is touched by both threads.
		if (!S->bDone.load(std::memory_order_acquire))
		{
			continue;
		}

		for (int32 I = 0; I < S->Pages.Num(); ++I)
		{
			const uint64 StageT0 = FPlatformTime::Cycles64();
			const int32 Base = StagePageStorage();
			FMemory::Memcpy(PendingDelta.StagedElevationMm.GetData() + Base,
			                S->Elev.GetData() + int64(I) * PixelsPerPage,
			                PixelsPerPage * sizeof(int32));
			FMemory::Memcpy(PendingDelta.StagedClimatePacked.GetData() + Base,
			                S->Climate.GetData() + int64(I) * PixelsPerPage,
			                PixelsPerPage * sizeof(uint32));
			WindowCost.StageCycles += FPlatformTime::Cycles64() - StageT0;
			CommitStagedPage(S->Pages[I].X, S->Pages[I].Y, Base);
			PagesInFlight.Remove(PageKey(S->Pages[I].X, S->Pages[I].Y));
		}
		AsyncPagesHarvested += uint64(S->Pages.Num());
		// The sampling buckets came from a WORKER. They are added to the same
		// window totals so the per-page breakdown is comparable across modes --
		// and the window line prints `ms GT` separately, measured on the game
		// thread only, so nobody can read worker milliseconds as game-thread
		// milliseconds. That distinction IS the mode-3 result.
		WindowCost.Add(S->Cost);
		S->Cost = FPageFillCost();
		S->Pages.Reset();
		S->bBusy = false;
	}
}

void FVoxelRasterAtlasCpu::LaunchAsyncSlots(vxc::ITileSampler& Tiles,
                                            int64 AnchorPxX, int64 AnchorPxY)
{
	constexpr uint32 PixelsPerPage = kPagePx * kPagePx;
	const int32 WantSlots = AsyncFillTasks();
	while (FillSlots.Num() < WantSlots)
	{
		FillSlots.Add(MakeUnique<FFillSlot>());
	}

	const int32 BatchPages = AsyncFillBatchPages();
	for (int32 SlotIdx = 0; SlotIdx < WantSlots; ++SlotIdx)
	{
		FFillSlot* S = FillSlots[SlotIdx].Get();
		if (S->bBusy)
		{
			continue;
		}

		TArray<FPagePt> Batch;
		Batch.Reserve(BatchPages);
		while (Batch.Num() < BatchPages)
		{
			FPagePt P;
			bool bFromDemand = false;
			if (!NextPageToFill(AnchorPxX, AnchorPxY, P, bFromDemand))
			{
				break;
			}

			// THE PRIME PROBE -- the guard that keeps a worker off the fatal
			// path. Four corners of the page, on the GAME THREAD, through the
			// same sampler: a page is 128 px and a fine tile is 8,192, so these
			// four pixels name exactly the set of tiles the page can span. A
			// non-resident tile takes ResolveNonResidentPixel's GAME-THREAD
			// branch here (block, load, correct) instead of the worker's
			// branch, which reports a gate leak and is FATAL unattended.
			// Four calls against the 32,768 the page will make: 0.01%.
			const int64 X0 = P.X * int64(kPagePx);
			const int64 Y0 = P.Y * int64(kPagePx);
			const int64 X1 = X0 + int64(kPagePx) - 1;
			const int64 Y1 = Y0 + int64(kPagePx) - 1;
			(void)Tiles.elevationMm(X0, Y0);
			(void)Tiles.elevationMm(X1, Y0);
			(void)Tiles.elevationMm(X0, Y1);
			(void)Tiles.elevationMm(X1, Y1);
			PrimeCalls += 4;

			if (bFromDemand)
			{
				PopDemandFront();
			}
			PagesInFlight.Add(PageKey(P.X, P.Y));
			Batch.Add(P);
		}

		if (Batch.Num() == 0)
		{
			return; // nothing left to fill; leave the remaining slots idle
		}

		S->Pages = MoveTemp(Batch);
		S->Elev.SetNumUninitialized(S->Pages.Num() * int32(PixelsPerPage));
		S->Climate.SetNumUninitialized(S->Pages.Num() * int32(PixelsPerPage));
		S->Cost = FPageFillCost();
		S->bDone.store(false, std::memory_order_relaxed);
		S->bBusy = true;
		++AsyncLaunches;

		const FVoxelRasterAtlasCpu* Self = this;
		vxc::ITileSampler* TilesPtr = &Tiles;
		// Raw captures, and safe for the same reason the asset-resolve warm
		// task's are (VoxelWorldSubsystem.cpp): the handle goes into the slot on
		// the next line and Shutdown waits on every busy slot before anything
		// this points at can be destroyed. A filler that was fired and forgotten
		// would outlive the sampler on teardown.
		//
		// BackgroundLow, matching the one launch pattern this module already
		// proves: a page is warmup, and it must never take a worker slot from a
		// chunk somebody is waiting for. If a mode-3 leg shows the fill
		// scheduler-starved (asyncPages far below launches x batch), the knob to
		// try next is BackgroundHigh, not more tasks.
		S->Task = UE::Tasks::Launch(
			TEXT("VoxelRasterAtlasFill"),
			[Self, S, TilesPtr]()
			{
				constexpr int64 PxPerPage = int64(kPagePx) * int64(kPagePx);
				for (int32 I = 0; I < S->Pages.Num(); ++I)
				{
					Self->SamplePage(*TilesPtr, S->Pages[I].X, S->Pages[I].Y,
					                 S->Elev.GetData() + int64(I) * PxPerPage,
					                 S->Climate.GetData() + int64(I) * PxPerPage,
					                 S->Cost);
				}
				S->bDone.store(true, std::memory_order_release);
			},
			UE::Tasks::ETaskPriority::BackgroundLow);
	}
}

void FVoxelRasterAtlasCpu::WaitForAsyncSlots()
{
	for (TUniquePtr<FFillSlot>& SlotPtr : FillSlots)
	{
		FFillSlot* S = SlotPtr.Get();
		if (S != nullptr && S->bBusy && S->Task.IsValid())
		{
			S->Task.Wait();
		}
		if (S != nullptr)
		{
			S->bBusy = false;
			S->Pages.Reset();
		}
	}
	PagesInFlight.Empty();
}

// ---------------------------------------------------------------------------

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
		// Whatever is in flight was sampled from the sampler we no longer trust.
		// Wait for it and throw it away rather than staging it.
		WaitForAsyncSlots();
	}
	if (bLoggedPitchMismatch)
	{
		LogWindowIfDue();
		return;
	}

	LastAnchorPxX = vxc::floorDiv(AnchorXMm, PixelSizeMm);
	LastAnchorPxY = vxc::floorDiv(AnchorYMm, PixelSizeMm);

	// EVERYTHING THE FILL COSTS THE GAME THREAD IS INSIDE THIS BRACKET, and
	// nothing else is. This is the number the registered disproof is written
	// against: mode 3 must drive it to ~0 while `asyncPages` proves the work
	// still happened. Measuring it here rather than summing the phase buckets
	// is deliberate -- in mode 3 the phase buckets are WORKER time, and adding
	// worker milliseconds into a game-thread total is exactly the mistake the
	// lock-contention null result was made of.
	const uint64 GtT0 = FPlatformTime::Cycles64();

	if (FillMode() >= 3)
	{
		HarvestAsyncSlots();
		LaunchAsyncSlots(Tiles, LastAnchorPxX, LastAnchorPxY);
	}
	else
	{
		const double Deadline = FPlatformTime::Seconds() + FillBudgetMs() / 1000.0;
		while (FPlatformTime::Seconds() < Deadline)
		{
			FPagePt P;
			bool bFromDemand = false;
			if (!NextPageToFill(LastAnchorPxX, LastAnchorPxY, P, bFromDemand))
			{
				break;
			}
			FillPage(Tiles, P.X, P.Y);
			if (bFromDemand)
			{
				PopDemandFront();
			}
		}
	}

	GtFillCycles += FPlatformTime::Cycles64() - GtT0;

	// Flush: one render command, enqueued BEFORE any dispatch this tick can
	// enqueue -- the ordering half of the mirror's soundness argument.
	if (PendingDelta.PageMeta.Num() > 0 || PendingDelta.InvalidateSlots.Num() > 0 ||
	    PendingDelta.bClearMissStats)
	{
		GpuAtlas.EnqueueUpsert(MoveTemp(PendingDelta));
		PendingDelta = FVoxelRasterAtlasGpuDelta();
	}

	ReactToClimateAudit();
	LogWindowIfDue();
}

void FVoxelRasterAtlasCpu::ReactToClimateAudit()
{
	// GAME THREAD ONLY. The workers only ever COUNT a mismatch; the decision to
	// stop trusting the dedup, and the log line, happen exactly once, here.
	if (bClimateDedupDisabled.load(std::memory_order_relaxed) || WindowCost.AuditMismatch == 0)
	{
		return;
	}
	bClimateDedupDisabled.store(true, std::memory_order_relaxed);
	UE_LOG(LogVoxelRasterAtlas, Error,
	       TEXT("[raster-atlas] FAIL: climate dedup audit MISMATCH -- %llu of %llu probes disagreed. ")
	       TEXT("-VoxelGpuRasterAtlasClimatePitchMm=%d gives a run of %lld px and that is NOT the run ")
	       TEXT("FineTileSampler::climate actually uses, so every page filled so far this session ")
	       TEXT("carries WRONG climate (wrong biome, wrong materials) over the deduped pixels. Dedup ")
	       TEXT("is now OFF for the session; pages filled from here are correct, pages already ")
	       TEXT("resident are NOT -- treat this leg as invalid and re-run with the right pitch."),
	       WindowCost.AuditMismatch, WindowCost.AuditChecked, ClimatePitchMm(), ClimateRunPx);
}

void FVoxelRasterAtlasCpu::LogWindowIfDue()
{
	const double Now = FPlatformTime::Seconds();
	const double Elapsed = Now - LastWindowLogSeconds;
	if (Elapsed < WindowSeconds())
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

	// The healthy line, unchanged in shape so every existing grep still lands.
	// Reading rules (also in the header): served=0 with the fork dispatching is
	// the DEAD reading; inlineFallback not falling toward zero after warmup
	// means prefetch is broken and the leg is measuring the control path
	// wearing the atlas flag.
	//
	// `ms GT` IS GAME-THREAD MILLISECONDS AND ONLY THOSE. In modes 0-2 it is
	// essentially the whole sampling cost; in mode 3 it is the harvest memcpys
	// and the prime probes and nothing else. That is the difference the mode
	// exists to make, and it must not be confused with the per-page cost on the
	// second line, which is wherever the sampling ran.
	UE_LOG(LogVoxelRasterAtlas, Log,
	       TEXT("[raster-atlas] window: served=%llu inlineFallback=%llu fills=%llu ")
	       TEXT("(%.2f MiB, %.1f ms GT) resident=%u/%u pages gpuMiss=%s lifetimeMiss=%llu win=%.2fs"),
	       Served, DeclinedCold, PagesFilled,
	       double(BytesUploaded) / (1024.0 * 1024.0),
	       MsFromCycles(GtFillCycles),
	       ResidentPages, PagesDim * PagesDim,
	       bHaveMiss ? *FString::Printf(TEXT("%u"), Miss.Misses) : TEXT("PENDING"),
	       GpuMissLifetime, Elapsed);

	// --- THE BREAKDOWN, with its residual -----------------------------------
	//
	// ABSOLUTES FIRST, SHARES SECOND, because a percentage without its absolute
	// is not a measurement -- that is the rule the lock-contention null result
	// bought this project (waitShare 99% of a denominator that never became
	// wall time). Every bucket prints ms/page AND % of the page.
	const double TotalMs = MsFromCycles(WindowCost.TotalCycles);
	const double ElevMs = MsFromCycles(WindowCost.ElevCycles);
	const double ClimMs = MsFromCycles(WindowCost.ClimateCycles);
	const double StageMs = MsFromCycles(WindowCost.StageCycles);
	const double ResidMs = TotalMs - (ElevMs + ClimMs + StageMs);
	const double Pages = double(FMath::Max<uint64>(WindowCost.Pages, 1));
	const double Pct = (TotalMs > 0.0) ? (100.0 / TotalMs) : 0.0;

	// THE TWO FAILURES THAT MUST NOT PRINT IDENTICALLY.
	FString Health;
	if (WindowCost.Pages == 0)
	{
		// Not a failure: the atlas has finished the disc and is idle. This is
		// the healthy steady state and the line says so in words so nobody
		// reads it as a dead instrument.
		Health = TEXT("IDLE (no pages filled this window -- steady state, not a dead counter)");
	}
	else if (WindowCost.ElevCalls == 0)
	{
		Health = TEXT("FAIL PHASE-NEVER-RAN: pages were filled and the elevation phase made ZERO ")
		         TEXT("sampler calls -- the instrument is outside the path, not a zero cost");
	}
	else if (WindowCost.ElevCycles == 0 || WindowCost.ClimateCycles == 0)
	{
		Health = TEXT("FAIL TIMER-DEAD: the phases ran (calls > 0) and a phase timer read zero ")
		         TEXT("cycles -- the timer is broken, the work is not free");
	}
	else if (FMath::Abs(ResidMs) > 0.20 * TotalMs)
	{
		Health = TEXT("SUSPECT RESIDUAL > 20%: the named buckets have stopped covering the work; ")
		         TEXT("a phase was added or skipped without a bucket");
	}
	else
	{
		Health = TEXT("ok");
	}

	// How many of the sweep's square pages the disc admits, recomputed at the
	// current anchor. Printed in EVERY mode (it is a property of the geometry,
	// not of the switch) because it is the denominator the traffic counter is
	// read against: mode 0's `lifetime fills` must converge on the SQUARE
	// count, mode >= 1's on the DISC count. Those two numbers not separating is
	// the reading that says mode 1 did not arm -- and it cannot be confused
	// with "the disc saved nothing", because the two totals are printed side by
	// side.
	int64 DiscPages = 0;
	int64 SquarePages = 0;
	for (int64 Dy = -RadiusPages; Dy <= RadiusPages; ++Dy)
	{
		for (int64 Dx = -RadiusPages; Dx <= RadiusPages; ++Dx)
		{
			++SquarePages;
			const int64 PxPage = vxc::floorDiv(LastAnchorPxX, int64(kPagePx)) + Dx;
			const int64 PyPage = vxc::floorDiv(LastAnchorPxY, int64(kPagePx)) + Dy;
			if (IsPageInCoverage(PxPage, PyPage, LastAnchorPxX, LastAnchorPxY))
			{
				++DiscPages;
			}
		}
	}

	const double ClimatePixels = double(FMath::Max<uint64>(WindowCost.ClimatePixels, 1));
	UE_LOG(LogVoxelRasterAtlas, Log,
	       TEXT("[raster-atlas] fill: mode=%s | %s | perPage %.3f ms = elev %.3f (%.0f%%) ")
	       TEXT("+ climate %.3f (%.0f%%) + stage %.3f (%.0f%%) + resid %.3f (%.0f%%) ")
	       TEXT("| callsPerPage elev=%.0f climate=%.0f of %.0f px (dedup %s, %.0fx) ")
	       TEXT("| audit checked=%llu mismatch=%llu | async pages=%llu launches=%llu inFlight=%d ")
	       TEXT("prime=%llu | disc %lld/%lld pages | lifetime fills=%llu win=%.2fs"),
	       FillModeName(FillMode()), *Health,
	       TotalMs / Pages,
	       ElevMs / Pages, ElevMs * Pct,
	       ClimMs / Pages, ClimMs * Pct,
	       StageMs / Pages, StageMs * Pct,
	       ResidMs / Pages, ResidMs * Pct,
	       double(WindowCost.ElevCalls) / Pages,
	       double(WindowCost.ClimateCalls) / Pages,
	       ClimatePixels / Pages,
	       (FillMode() < 2)         ? TEXT("off")
	       : bClimateDedupDisabled.load(std::memory_order_relaxed)
	                                ? TEXT("DISABLED-BY-AUDIT")
	       : (ClimateRunPx > 1)     ? TEXT("on")
	                                : TEXT("UNAVAILABLE"),
	       double(WindowCost.ClimatePixels) / double(FMath::Max<uint64>(WindowCost.ClimateCalls, 1)),
	       WindowCost.AuditChecked, WindowCost.AuditMismatch,
	       AsyncPagesHarvested, AsyncLaunches, PagesInFlight.Num(), PrimeCalls,
	       DiscPages, SquarePages, PagesFilledLifetime, Elapsed);

	// THE SECOND GAME-THREAD RASTER TERM, and the split the submit line cannot
	// make. `Voxel gpu submit split`'s `raster=` brackets this call AND the
	// inline FillRasterWindow a decline falls back to; subtracting `prepareMs`
	// from it leaves the inline half, which is what a faster warmup DELETES
	// rather than moves. Printed with its call count so a per-call figure is
	// available without dividing by the wrong denominator.
	const double PrepMs = MsFromCycles(PrepareCycles);
	UE_LOG(LogVoxelRasterAtlas, Log,
	       TEXT("[raster-atlas] prepare: calls=%llu prepareMs=%.1f (%.2f us/call) declines=%llu ")
	       TEXT("-- subtract prepareMs from `Voxel gpu submit split`'s raster= to get the inline ")
	       TEXT("FillRasterWindow half, which is per-DECLINE and is deleted by warming sooner, ")
	       TEXT("not moved. calls=0 with the fork dispatching is the DEAD reading. win=%.2fs"),
	       PrepareCalls, PrepMs,
	       (PrepareCalls > 0) ? (PrepMs * 1000.0 / double(PrepareCalls)) : 0.0,
	       DeclinedCold, Elapsed);

	Served = 0;
	DeclinedCold = 0;
	PagesFilled = 0;
	BytesUploaded = 0;
	AsyncPagesHarvested = 0;
	AsyncLaunches = 0;
	PrimeCalls = 0;
	PrepareCycles = 0;
	PrepareCalls = 0;
	GtFillCycles = 0;
	WindowCost = FPageFillCost();
}

bool FVoxelRasterAtlasCpu::PrepareRequest(FVoxelGpuRegionRequest& Req)
{
	check(IsInGameThread());
	check(IsInitialized());
	if (bLoggedPitchMismatch)
	{
		return false;
	}
	const uint64 PrepT0 = FPlatformTime::Cycles64();
	++PrepareCalls;
	ON_SCOPE_EXIT { PrepareCycles += FPlatformTime::Cycles64() - PrepT0; };

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
			// window and dispatches this tick. A page already handed to a
			// worker is skipped: it is coming, and queueing it again would put
			// two entries in the demand queue for one page.
			const uint64 Key = PageKey(Px, Py);
			if (!DemandQueued.Contains(Key) && !PagesInFlight.Contains(Key))
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
	// FIRST, ALWAYS, AND BEFORE THE IsInitialized EARLY RETURN COULD SKIP IT:
	// a filler task holds raw pointers to this object and to the tile sampler.
	// The release below is the last thing this class does, so this is the last
	// place the wait can happen.
	WaitForAsyncSlots();
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
