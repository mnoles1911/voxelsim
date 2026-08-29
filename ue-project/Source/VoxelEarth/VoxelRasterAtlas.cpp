// FVoxelRasterAtlasCpu -- policy side of the persistent GPU raster atlas.
// See the header for the design, the fill-mode ladder and the failing
// readings; this file is the mechanics: margin derivation THROUGH the one
// window rule, nearest-first fills under a time budget, the coverage check,
// the per-page cost breakdown, and the stats window.

#include "VoxelRasterAtlas.h"

#include "VoxelGpuWorldGen.h"

#include "VoxelFineTileStreamer.h"  // kDefaultFineRingRadiusTiles -- the pin mode 3 rides on

#include "voxelcore/core.h"
#include "voxelcore/tilestore.h"      // vxc::tilePixelSizeMm -- the climate-pitch default
#include "voxelcore/tilestreaming.h"  // vxc::tileCoordForPixel -- THE page->tile rule, not a restatement

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

	// --- THE DEMAND-FILL CAP, IN PAGES PER STREAMING TICK -------------------
	//
	// FillWindowOnDemand (see its comment in the header) rescues a declined
	// chunk by filling its window's missing pages instead of letting it sample
	// a 5,800-pixel window inline. Uncapped, that trades one unbounded
	// game-thread cost for another: on the 240 m page crossing that produces
	// the measured 10.2 s hitch, 7-25 chunks decline in ONE tick, and a tick
	// that filled every page all of them wanted would simply be a differently
	// shaped 240 ms.
	//
	// PAGES, NOT MILLISECONDS, and deliberately so. FillBudgetMs() is a time
	// budget tested BEFORE a page, and its own comment records what that costs:
	// a tick with 0.1 ms left still pays a whole page, so the "2.0 ms" budget
	// measures 2.83 ms/page in practice. A count is the unit that actually
	// bounds the tick here -- 4 pages x ~1.5 ms measured
	// (`fills=16 (2.00 MiB, 23.9 ms GT)`) is ~6 ms of game thread, and it
	// cannot overshoot by a page the way a deadline can.
	//
	// 4 IS A STARTING VALUE, not a derived one, and it is switchable precisely
	// so it does not need to be defended as derived. A level-0 window is
	// ~76 px across and spans at most 2x2 = 4 pages, so 4 admits a whole
	// level-0 window in one call -- which is the case that matters, because a
	// crossing is many chunks sharing a few pages, not one chunk wanting many.
	// A level-6 window is ~64x wider in world terms and can span 3x3; those
	// windows will hit the cap, take the inline path, and be picked up by the
	// sweep's demand queue on a later tick, which is exactly today's behaviour
	// and therefore not a regression.
	//
	// THE REFUSAL IS ALL-OR-NOTHING, and that is the whole reason the cap is
	// safe. A partial fill would leave the window still uncovered, so the chunk
	// would pay the pages AND the inline window -- strictly worse than today.
	// Refusing outright costs the chunk exactly what it costs now, and the
	// pages it wanted are already in the demand queue (PrepareRequest queued
	// them when it declined, before this was ever called), so the sweep fills
	// them next tick and the NEXT chunk over that ground is served.
	//
	// 0 disables the demand path entirely -- the A/B control arm, and the way
	// to reproduce the pre-fix hitch on a build that has the fix in it.
	int32 DemandPagesPerTick()
	{
		static const int32 Value = []
		{
			// DEFAULT 64, MEASURED 2026-08-25. It took three legs to find this and the
			// two smaller values LOOKED LIKE THE FIX HAD FAILED, so the numbers stay here:
			//
			// Swept twice, because the FIRST sweep was run with
			// -VoxelGpuMeshBatchCap=16 passed and therefore never saw the SHIPPING
			// MeshBatchCap of 64, which promotes far more chunks per tick and so
			// brings far more declines due at once. A fix measured under a
			// non-default flag has not been measured.
			//
			//   at MeshBatchCap 16:  cap 4 -> rescued 6/capHit 14;  16 -> 118/119;
			//                        64 -> 150/0
			//   at MeshBatchCap 64 (THE DEFAULT, what actually ships):
			//     cap   capHit  rescued  noAtlas  worstFrame  hitch time / count / >=200ms
			//      64     1070      264     1070     98.65 ms   4058 ms / 40 / 4
			//     256        0      548        0     87.42 ms   2544 ms / 31 / 1   <- ships
			//    1024        0      523        0     91.17 ms   3074 ms / 38 / 2
			//
			// 1024 is WORSE than 256: an effectively unbounded cap lets a single
			// tick fill hundreds of pages and rebuilds a stall of its own. The cap
			// exists to RATION, not to permit.
			//
			// Against pre-fix stock: total time in hitch frames 4795 -> 2544 ms
			// (-47%), frames >= 200 ms 10 -> 1, worst moving frame 242 -> 87 ms.
			//
			// Below 64 the cap REFUSED most page-column crossings, they fell back to
			// the per-chunk inline window, and the 10.2 s metronome survived intact --
			// the fix reads as ineffective when it is simply rationed. `capHit` is the
			// discriminator: a leg with capHit comparable to rescued is CAP-BOUND, not
			// a null result. At 64, capHit=0 and noAtlas fell 1196 -> 0.
			//
			// A crossing needs far more than a handful of pages at once: a page is
			// 128 px x 1875 mm/px = 240 m, so at 20+ m/s one is crossed every ~10 s
			// and a whole page COLUMN comes due in one tick.
			//
			// WHY NOT ASYNC (-VoxelGpuRasterAtlasFill=3) INSTEAD: measured, and it is
			// WORSE despite moving 85% of the fill off the game thread (489 -> 79 ms
			// per 2 s window). It SPREADS the stall rather than removing it. Total time
			// in hitch frames over one flight: baseline 4795 ms / 35 hitches, this fix
			// 3957 ms / 58, async 5834 ms / 77. Severity, frames >= 200 ms: baseline 10,
			// this fix 1, async 4. The game-thread number alone would have sold async;
			// summing the hitch time is what refuted it.
			//
			// WHERE THE RESIDUAL LANDS, AND IT IS NOT THE TAIL (2026-08-26,
			// TJDL-A-control.log, flag-free shipping default). This budget was
			// re-opened on the theory that its remaining freeze -- a hitch frame
			// of dispatchMs=117.18 of which submitMs=116.32 -- was the settled
			// -MOVING tail. IT IS NOT, and the leg says so three ways:
			//
			//   * EVERY dispatch line on that leg with submitMs above 4 ms is
			//     timestamped 13:04:11.77 .. 13:04:14.27. The settle BOUNDARY is
			//     13:04:17.87. All of them are FILL frames.
			//   * The five post-settle dispatch lines carry submitMs = 0.00 /
			//     0.02 / 0.00 / 0.05 / 0.00.
			//   * Post-settle this atlas spends 943 ms of game thread TOTAL over
			//     130 windows (fill 734.3 + demand 209.0), against a settled
			//     -moving population of 12,638 frames whose excess mass above p50
			//     is >= 3,850 ms -- and the game thread on those frames is
			//     already idle 6.29 ms of 9.67 (RENDERBOUND), so a game-thread
			//     term buys nothing until it exceeds that slack.
			//
			// THE READING THAT MISLED, so it is not repeated: `Hitch frame
			// dispatch` only prints above 33.3 ms, and the settled-moving tail is
			// p95=15.20 / p99=19.60 -- entirely UNDER that bar. Grepping submitMs
			// off the hitch lines therefore cannot describe the tail at all; it
			// can only ever describe the fill. Use
			// voxel.Stream.FrameAttribution=2, which samples every settled-moving
			// frame and carries submit with its MAX.
			//
			// So the residual here costs COLD START, not frame tail: the atlas
			// spends ~1,464 ms of game thread (fill 781.3 + demand 682.6) inside
			// a 6.1 s settle, i.e. ~24% of a settle whose standing target is
			// < 5 s. That is the project this budget belongs to.
			//
			// AND THE CAP IS INERT AT 256: capHit=0 (lifetime 0) across the whole
			// leg, and the demand path averages 1.29 pages per CALL (450 calls,
			// 579 pages in its worst window) with a per-call maximum of 3.22. The
			// freeze is the SUM of many one-page calls inside a single dispatch
			// loop, not one call filling a column, so 256 is roughly 3x above any
			// freeze that has occurred and never binds. Lowering it does not
			// shorten the freeze proportionally -- it pushes chunks to the inline
			// path, which is the capHit=1070/noAtlas=1070 failure at 64 recorded
			// above. The lever is exhausted in both directions.
			int32 V = 256;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuRasterAtlasDemandPagesPerTick="), V);
			return FMath::Max(0, V);
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
			if (V > 0)
			{
				return V;   // explicitly TOLD; outranks the derivation below
			}
			// DERIVED, NOT GUESSED, and this is what makes fill mode 2 safe to
			// default. The comment above is right that this file must not
			// restate FineTileSampler's /16 -- so it does not. It reads the
			// SAME switch the coarse sampler is constructed from
			// (-VoxelTileScale, VoxelWorldSubsystem.cpp:1415-1517, default 1)
			// and asks voxelcore for that scale's pitch. If the two ever
			// disagree it is because someone changed the sampler's scale
			// without changing this switch, which is the one thing the switch
			// exists to say.
			//
			// WHY IT HAD TO CHANGE BEFORE MODE 2 COULD SHIP: the default was 0
			// = "not told", and `bDedup` requires ClimateRunPx > 1, so
			// defaulting the fill mode to 2 with the pitch still 0 would have
			// shipped the dedup PERMANENTLY OFF while the mode said it was on.
			// That is the inert-feature shape this project has now found twelve
			// times, and the window line would have read
			// `climateDedup=UNAVAILABLE` on every shipped run.
			//
			// An unsupported scale returns 0 from tilePixelSizeMm, which lands
			// back on "not told" -- dedup off, per-pixel fills, correct. The
			// 64-probe audit still runs on every page and still disables the
			// dedup for the session on any mismatch, so a derivation that is
			// somehow wrong is caught rather than written into the atlas.
			int32 TileScale = 1;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelTileScale="), TileScale);
			return int32(vxc::tilePixelSizeMm(uint8(TileScale)));
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
	// into a killed leg. The FIRST half of the guard -- necessary, and on its
	// own NOT sufficient; the section below is the other half -- is in
	// LaunchAsyncSlots: every page is PRIMED
	// on the game thread with four corner samples first -- a page is 128 px and
	// a fine tile is 8,192, so four corners name exactly the tiles the page can
	// span -- which forces the game-thread branch and leaves the worker reading
	// resident tiles only. Four calls per page against 32,768 is 0.01%.
	//
	// THE PRIME IS NOT A PROOF. WHAT CLOSES IT (2026-08-24) ------------------
	//
	// The residual risk, stated as it always was: TickResidencyAndEviction can
	// evict a tile BETWEEN the prime and the worker's read of it, and a worker
	// reading an evicted tile is the fatal branch. The old note answered that
	// with "with ringRadius=0 and 1.82 of a 12.00 GiB budget resident, no
	// eviction happens on a cold start" -- which is an observation about one
	// leg's headroom, not a property of the code. A long flight is precisely
	// the run that spends that headroom, and an unattended leg that dies at
	// minute nine loses the whole leg. So the prime alone can never be the
	// guard, and mode 3 was correctly kept off while it was.
	//
	// WHAT WAS TRIED FIRST AND IS NOT POSSIBLE FROM THIS FILE, recorded so the
	// next person does not spend the afternoon rediscovering it. The obvious
	// design is: a worker that finds a non-resident pixel ABANDONS the page and
	// requeues it for a game-thread fill -- a fatal race becomes a rare slow
	// path, and nothing is lost because the game-thread path blocks on a
	// synchronous load and is documented as "slow, but correct". It cannot be
	// written here. The atlas holds a vxc::ITileSampler&, whose entire surface
	// is pixelSizeMm / elevationMm / climate (voxelcore/tiles.h) -- no try-form,
	// no residency query. DISCOVERING that a pixel is non-resident and TRIPPING
	// the fatal report are therefore the SAME instruction, and both live in
	// another file: elevationMm -> ResolveNonResidentPixel -> its worker branch
	// -> ReportGateLeak_Locked. There is no signal to abandon on. (The
	// abandonment design is still the right one; see the last paragraph for the
	// six lines that would create the signal.)
	//
	// SO THE GUARD IS A PIN INSTEAD -- and the residency system already has
	// one; it just is not spelled as an API. TickResidencyAndEviction calls
	// Budget_.setPinned(squareTileRing(anchorTile, R)) every tick, and
	// LruBudgetCache::selectEvictions() excludes every pinned key "even if that
	// leaves the cache over budget" (voxelcore/tilestreaming.h). A tile inside
	// the ring CANNOT be evicted, budget pressure or not. This class cannot
	// take a pin of its own, but it can confine its workers to pages that are
	// already inside somebody else's, which is exactly as strong.
	//
	// THE PROOF, in the four steps it actually rests on:
	//   1. ORDER. TickResidencyAndEviction runs inside RecomputeDesiredSet,
	//      which is EARLIER in the same streaming tick than this atlas's Tick
	//      (VoxelWorldSubsystem.cpp: the residency call, then the raster-atlas
	//      block placed ahead of DispatchJobs). Every eviction of tick N is
	//      already behind us when we launch; the next one cannot happen until
	//      tick N+1.
	//   2. ADMISSION. A page is handed to a worker only if EVERY fine tile it
	//      can touch lies within Chebyshev R-1 of the anchor's tile, R being
	//      the streamer's ring radius (-VoxelFineTileRingRadius, default 0).
	//      Four corners name exactly the tiles a page can span, for the same
	//      arithmetic the prime uses: 128 px page, 8,192 px tile.
	//   3. ONE CROSSING OF SLACK. The anchor moves at most one coarse tile per
	//      tick (a tile is 15.36 km; even 240 m/s is ~4 m per frame), so tick
	//      N+1's ring is centred at most one tile away and still contains
	//      everything within R-1 of tick N's centre. Pages in flight are still
	//      pinned through the first eviction pass that could see them.
	//   4. THE BARRIER. That slack is spent after ONE crossing, so the atlas
	//      DRAINS -- waits out every busy slot and harvests it -- the moment it
	//      sees the anchor's coarse tile change. By (1) the drain happens in the
	//      same tick as the eviction pass that moved the ring, so no worker
	//      survives into the second one.
	// The other eviction site cannot reach us either: EnsureTileResident_Locked
	// unloads only a tile it JUST loaded and failed to decode, and it returns
	// early on an already-resident tile, so it can never unload one a worker is
	// reading.
	//
	// WHAT R=0 MEANS, said plainly rather than hidden in the arithmetic: R-1 is
	// -1, so the default admits NOTHING. With only the anchor's own tile pinned
	// there is no page whose tiles survive a crossing. Mode 3 then fills
	// synchronously and the window line SAYS SO (`asyncSafety=`, `syncFallback=`
	// and `asyncPages=0` together). Measuring mode 3 on a fine-tier leg
	// therefore requires -VoxelFineTileRingRadius=1 AND a cache budget that
	// holds a 3x3 of decoded tiles. That is a real constraint on the experiment,
	// not a formality, and a leg that forgets it measures mode 2 wearing mode
	// 3's name.
	//
	// THE BARRIER'S COST, UNMEASURED AND SAID SO: a drain waits out whatever is
	// left of a batch (tasks x batch pages of worker sampling) on the GAME
	// THREAD, once per coarse-tile crossing -- one crossing per 15.36 km, so
	// ~once per 64 s at 240 m/s and ~once per 12.8 min at the frame gate's
	// 20 m/s. It is printed as `drains=N (... ms GT)` because a safety path
	// that costs the tail must show its bill. If that ms ever becomes a visible
	// share of a window, shrink -VoxelGpuRasterAtlasFillBatch; do not remove
	// the barrier.
	//
	// WHAT WOULD BE BETTER, AND IT IS NOT IN THIS FILE. Six lines in
	// VoxelFineTileStreamer beat all of the above:
	//     bool FVoxelFineTileSamplerProxy::tryElevationMm(px, py, int32_t& Out)
	// answering FALSE on a worker instead of calling ReportGateLeak_Locked.
	// The atlas would then abandon the page, requeue it at demand priority and
	// let the game thread fill it correctly -- no ring requirement, no barrier,
	// no drain, and the fatal race unreachable by construction rather than by
	// argument. A pin/refcount API is the WORSE of the two options and should
	// not be built: it makes the game thread's eviction wait on a worker, which
	// is the same tail cost mode 3 exists to remove, and Budget_::setPinned is
	// REPLACED wholesale every residency tick, so a pin added naively beside it
	// would be silently wiped -- inert while looking armed, which is the trap
	// this file already documents twice.
	//
	// -VoxelGpuRasterAtlasFillSafety=N, latched:
	//   0  OFF -- prime and hope. Exactly the arm the ladder's mode-3 row was
	//      measured on, kept so that measurement can be reproduced, and it can
	//      still kill an unattended run.
	//   1  PINNED-RING ADMISSION + drain barrier (DEFAULT). Safe by the proof
	//      above. Pages it refuses are filled SYNCHRONOUSLY in the same tick
	//      under the ordinary budget, so warmup still finishes and the refusal
	//      costs correctness nothing.
	int32 AsyncSafetyMode()
	{
		static const int32 Value = []
		{
			int32 V = 1;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuRasterAtlasFillSafety="), V);
			return FMath::Clamp(V, 0, 1);
		}();
		return Value;
	}

	// The streamer's ring radius, read from the SAME switch the streamer is
	// constructed from (VoxelWorldSubsystem.cpp's FineRingRadius: <0 leaves
	// FVoxelFineTileStreamer::kDefaultFineRingRadiusTiles). This is the rule
	// ClimatePitchMm() already follows -- read the producer's switch, never
	// restate the producer's policy -- and it matters more here than there,
	// because a value that is too LARGE makes the admission test wrong in the
	// UNSAFE direction. So the window line prints the number this file used and
	// the streamer prints its own on the `Fine tier ENABLED:` startup line: two
	// independent spellings of one fact, in one log, greppable side by side.
	int32 FineRingRadiusTiles()
	{
		static const int32 Value = []
		{
			int32 V = -1;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelFineTileRingRadius="), V);
			return (V >= 0) ? V : int32(FVoxelFineTileStreamer::kDefaultFineRingRadiusTiles);
		}();
		return Value;
	}

	// CAN A WORKER OF OURS REACH THE FATAL GATE THROUGH THIS SAMPLER?
	//
	// FAIL-CLOSED, AND BY PITCH RATHER THAN BY TYPE, because this module is
	// compiled /GR- (bUseRTTI defaults false -- VoxelWaterSubsystem.cpp records
	// the same constraint at its own type test), so dynamic_cast is not
	// available. The only sampler that can leak is FVoxelFineTileSamplerProxy,
	// and it is the only sampler this project puts on ActiveTiles() at
	// 1,875 mm/px. The alternatives are worker-safe at any pitch and say so
	// themselves: vxc::TileGridSampler is immutable after init ("tiles_ is
	// populated only during init ... otherwise pure reads of immutable data",
	// tilestore.h) and vxc::SyntheticTileSampler is a pure function of a seed.
	//
	// THE ONE FALSE POSITIVE IS THE SAFE DIRECTION: a coarse TileGridSampler
	// run at -VoxelTileScale=16 is also 1,875 mm/px and would be treated as
	// leak-capable. That costs async admission on a configuration nothing in
	// this repo uses. The opposite error costs a leg.
	//
	// WHAT WOULD REFUTE IT: an ITileSampler that mutates on query and does NOT
	// report 1,875 mm/px would be classified safe and would be wrong. That is
	// why the answer is printed (`asyncSafety=` on the window line) instead of
	// being assumed -- a run states which classification it got.
	bool SamplerCanLeakOnWorker(const vxc::ITileSampler& Tiles)
	{
		return Tiles.pixelSizeMm() == vxc::tilePixelSizeMm(vxc::kFineTileScale);
	}

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
		// DEFAULT 1 AS OF 2026-08-24. The fill-mode default of 2 shipped hours
		// earlier was INERT without this master switch: a leg run with no atlas
		// flag prints no [raster-atlas] line at all, so the disc sweep and the
		// climate dedup never ran. That is the inert-feature trap -- the one I
		// have caught in three other people's work tonight -- in my own.
		// Reverted with -VoxelGpuPrimary. The atlas serves the GPU fork's region
		// requests; with the fork off there is little for it to serve, and it
		// was never measured on the stock base. Its fill modes 1+2 remain
		// correct and default-2 -- they simply do not run until the atlas does.
		//
		// RE-ARMED 2026-08-24 with the rest of the GPU-primary set, by owner
		// decision. The reason it was reverted is also the reason re-arming it
		// is coherent: it serves the GPU fork's region requests, and the fork is
		// now on by default, so there IS something for it to serve again.
		//
		// FillMode() below stays at 2. MODE 3 (async fill) REMAINS OFF AND IS
		// NOT PART OF THIS ARMING -- a worker touching a non-resident pixel
		// reaches ReportGateLeak_Locked, which is FATAL on an unattended run,
		// and no arm on that ladder was ever measured against the moving-segment
		// frame gate. Arming the master switch does NOT arm mode 3; if a future
		// change makes it do so, that is a defect.
		//
		// FAILING READINGS, both ways: a leg on this default that prints NO
		// [raster-atlas] line at all means the master switch is inert again (the
		// exact trap this comment was written about); and a leg that prints the
		// line with mismatch>0 has had its climate dedup disabled for the
		// session and its per-page figures are not comparable to a clean arm.
		int32 Value = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuRasterAtlas="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

int32 FVoxelRasterAtlasCpu::FillMode()
{
	static const int32 Value = []
	{
		// DEFAULT 2 (disc sweep + climate dedup) AS OF 2026-08-23. Mode 3
		// (async fill) is DELIBERATELY NOT the default -- see below.
		//
		// SHIPPED ON THIS LADDER (atl-f0..f3, matched arms, one switch each):
		//   mode  perPage    elev            climate         fill ms GT  settle
		//     0   1.093 ms   0.642 (59%)     0.445 (41%)       4,363 ms   21.4 s
		//     1   0.971      0.563           0.397             3,071      20.9
		//     2   0.587      0.572 (97%)     0.006 ( 1%)       1,877      20.9
		//     3   0.902      0.894           0.008               330      20.3
		// Mode 1 is a counted 1,020-of-1,521-page saving (the corner pages of
		// the square sweep are outside the coverage disc and were being filled
		// for nothing). Mode 2 collapses the climate term 98.7% by sampling
		// once per coarse run instead of once per pixel -- callsPerPage climate
		// 16,384 -> 64, a 256x reduction -- and its audit re-samples 64 probes
		// per page on every leg and disables the dedup for the session on any
		// mismatch. mismatch=0, gateLeaks=0, holes=0 on every arm, every ring
		// improved and none lost.
		//
		// WHY NOT 3, given it is the fastest arm. Two reasons were recorded.
		// BOTH HAVE NOW MOVED, and neither is deleted here, because an
		// objection that has been overtaken should be written down as overtaken
		// rather than quietly dropped -- otherwise the next person cannot tell
		// a resolved risk from one nobody thought about.
		//
		// (1) THE CONCURRENCY RISK -- "a worker touching a non-resident pixel
		//     reaches ReportGateLeak_Locked, which is FATAL on an unattended
		//     run, and the four-corner prime that guards it can be defeated by
		//     a racing eviction." STILL REAL, and now GUARDED rather than
		//     hoped about: -VoxelGpuRasterAtlasFillSafety=1 (the default)
		//     admits a page to a worker only when every fine tile it can touch
		//     is inside the streamer's PINNED ring with a tile of margin, and
		//     drains every worker the moment the anchor crosses a tile. The
		//     four-step proof, its cost, and the streamer-side change that
		//     would beat it are at AsyncSafetyMode() above. The price is that
		//     the default ring radius (0) admits nothing, so mode 3 on a
		//     fine-tier leg needs -VoxelFineTileRingRadius=1 to be measuring
		//     anything at all.
		//
		// (2) "No arm in this ladder was measured against the >100
		//     FPS-after-settle goal, and adding threads without a
		//     settled-segment p95 instrument is exactly the trade that goal
		//     exists to prevent." THIS OBJECTION IS OVERTAKEN: that instrument
		//     now exists. `Voxel frame dist seg=SETTLED-MOVING scope=total`
		//     reports p50/p95/p99/max and stutter counts over the settled
		//     moving segment, and it is what the owner's frame gate is read
		//     from. The gate itself changed too -- it is no longer a stutter
		//     percentage but "1% low (p99 frame time) >= 50 fps while moving at
		//     >= 20 m/s" -- so the TAIL is the target, and mode 3's case is now
		//     a tail case rather than a settle-time case.
		//
		// WHY THE TAIL IS WHERE MODE 3 IS SUPPOSED TO PAY. Measured on the
		// 2026-08-25 EE-default leg, and NOT BY ME -- I cannot run a leg from
		// here, so nothing in this comment is a number I produced: the worst
		// frames of a flight are game-thread stalls inside GPU job submission
		// (a hitch frame reported frameMs=400.00, itself a clamp of a 1,104.66
		// ms tick, of which dispatchMs=1093.47 and submitMs=1089.77). submitMs
		// is bimodal -- n=96, p50=0.00, p90=0.60, max=1,089.77 ms -- and the
		// submit bracket covers the GPU fork's region request, which pulls
		// raster-atlas pages that fill SYNCHRONOUSLY on the game thread. The
		// same log's per-window atlas cost was 385.0 / 245.4 / 217.7 / 156.1 ms
		// at perPage 2.736 ms = elev 2.699 (99%). That is the term mode 3 moves
		// off the tick.
		//
		// AND THE HONEST NUMBER, WHICH CUTS AGAINST ALL OF THAT: removing
		// 4,033 ms of game thread bought 1.1 s of settle -- 27%, not 1:1.
		// Game-thread milliseconds are not wall milliseconds. The claim above
		// is about the TAIL and that caveat is about SETTLE TIME, and they are
		// different quantities -- but the caveat is the reason the tail claim
		// is a hypothesis and not a plan: work removed from the game thread has
		// already been measured, on this very feature, NOT to convert into wall
		// time at anything like 1:1.
		//
		// THE FALSIFIER FOR ARMING MODE 3, registered before the leg (the
		// owner's, 2026-08-24). With mode 3 armed:
		//   * `gateLeaks` must be 0 AND `mismatch` must be 0 across a full
		//     flight leg. A non-zero gateLeaks means the safety path is NOT
		//     covering the race and this change is WRONG -- not tunable,
		//     wrong -- and mode 3 goes back off immediately.
		//   * the settled-moving p99 must IMPROVE. If it does not, then moving
		//     ~1.5 s of game thread off the tick did not reach the tail, mode 3
		//     stays off regardless of how safe it is, and the tail is somewhere
		//     else -- which is a result worth the leg either way.
		//   * `asyncPages` must be > 0. Zero means the ring/admission gate
		//     refused everything (read `asyncSafety=` and `declinedUnpinned=`
		//     on the atlas window line) and the leg measured mode 2.
		int32 V = 2;
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

bool FVoxelRasterAtlasCpu::AnyAsyncSlotBusy() const
{
	for (const TUniquePtr<FFillSlot>& SlotPtr : FillSlots)
	{
		if (SlotPtr.IsValid() && SlotPtr->bBusy)
		{
			return true;
		}
	}
	return false;
}

// STEP 2 OF THE PROOF at AsyncSafetyMode(). Only the arithmetic lives here.
bool FVoxelRasterAtlasCpu::IsPageAsyncSafe(int64 PageX, int64 PageY) const
{
	if (AsyncSafetyMode() == 0 || !bAsyncSamplerCanLeak)
	{
		// Either the operator explicitly asked for the unguarded arm, or this
		// sampler has no worker path into ReportGateLeak_Locked at all
		// (immutable coarse grid / pure synthetic), in which case there is
		// nothing to be safe FROM and confining the sweep would only cost.
		return true;
	}

	// R-1, not R: the anchor can cross ONE tile before the barrier fires, and
	// the ring that will exist AFTER that crossing is the one that has to still
	// contain this page. R=0 therefore admits nothing, which is the honest
	// answer for a run whose streamer pins only the tile under the player.
	const int32 Margin = FineRingRadiusTiles() - 1;
	if (Margin < 0)
	{
		return false;
	}

	// vxc::tileCoordForPixel, NOT floorDiv(px, 8192) written out again: the
	// page->tile mapping has exactly one spelling and the streamer's own gate
	// reads it from there too. A second copy here is the D5 drift this whole
	// class is built to avoid.
	const int64 TileSizePx = int64(vxc::kFineTileSize);
	const vxc::TileCoord AnchorTile = vxc::tileCoordForPixel(LastAnchorPxX, LastAnchorPxY, TileSizePx);
	const int64 X0 = PageX * int64(kPagePx);
	const int64 Y0 = PageY * int64(kPagePx);
	const int64 X1 = X0 + int64(kPagePx) - 1;
	const int64 Y1 = Y0 + int64(kPagePx) - 1;
	// Four corners name EVERY tile the page can span, by the same arithmetic
	// the prime probe uses: a page is 128 px, a fine tile is 8,192.
	const vxc::TileCoord Corners[4] = {
		vxc::tileCoordForPixel(X0, Y0, TileSizePx),
		vxc::tileCoordForPixel(X1, Y0, TileSizePx),
		vxc::tileCoordForPixel(X0, Y1, TileSizePx),
		vxc::tileCoordForPixel(X1, Y1, TileSizePx)};
	for (const vxc::TileCoord& T : Corners)
	{
		const int64 Dist = FMath::Max(FMath::Abs(int64(T.x) - int64(AnchorTile.x)),
		                              FMath::Abs(int64(T.y) - int64(AnchorTile.y)));
		if (Dist > int64(Margin))
		{
			return false;
		}
	}
	return true;
}

void FVoxelRasterAtlasCpu::DrainAsyncSlots()
{
	for (TUniquePtr<FFillSlot>& SlotPtr : FillSlots)
	{
		FFillSlot* S = SlotPtr.Get();
		if (S != nullptr && S->bBusy && S->Task.IsValid())
		{
			S->Task.Wait();
		}
	}
	// HARVEST, do not discard. Everything these workers sampled was read while
	// its tiles were still pinned (step 3 of the proof), so the pages are
	// correct and throwing them away would spend the barrier's cost twice.
	// Task.Wait() has returned, so bDone's release store is visible and
	// HarvestAsyncSlots' acquire load will see every slot finished.
	HarvestAsyncSlots();
}

void FVoxelRasterAtlasCpu::FillDeferredPagesSync(vxc::ITileSampler& Tiles, const TArray<FPagePt>& Pages)
{
	if (Pages.Num() == 0)
	{
		return;
	}
	// They were marked in-flight only so the sweep would step past them while
	// the batch was being built; they are not in flight and nobody is filling
	// them but us. Clear the marks BEFORE filling so that an exhausted budget
	// cannot leave a page permanently unfillable.
	for (const FPagePt& P : Pages)
	{
		PagesInFlight.Remove(PageKey(P.X, P.Y));
	}

	// The ordinary budget, and the ordinary overshoot: the deadline is tested
	// BEFORE a page, never during, so this pays at most one whole page past it
	// -- exactly as the synchronous modes do. That is deliberate. When the
	// admission test refuses everything (ring radius 0), mode 3's game-thread
	// cost must land on mode 2's number rather than on some third value nobody
	// can compare, or the ladder stops being a ladder.
	const double Deadline = FPlatformTime::Seconds() + FillBudgetMs() / 1000.0;
	for (const FPagePt& P : Pages)
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			break;
		}
		if (IsPageResident(P.X, P.Y))
		{
			continue;
		}
		FillPage(Tiles, P.X, P.Y);
		++AsyncSyncFallbackPages;
	}
}

void FVoxelRasterAtlasCpu::LaunchAsyncSlots(vxc::ITileSampler& Tiles,
                                            int64 AnchorPxX, int64 AnchorPxY,
                                            TArray<FPagePt>& OutDeferred)
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
		// BOUNDED SCAN, and the bound is the same one that was always here.
		// Every candidate the admission test refuses consumes one of this
		// batch's attempts instead of restarting the search, so a leg where
		// NOTHING is admissible (ring radius 0) costs the same tasks x batch
		// sweep probes per tick it costs today -- it does not walk the disc.
		for (int32 Considered = 0; Considered < BatchPages && Batch.Num() < BatchPages; ++Considered)
		{
			FPagePt P;
			bool bFromDemand = false;
			if (!NextPageToFill(AnchorPxX, AnchorPxY, P, bFromDemand))
			{
				break;
			}

			// THE SAFETY ADMISSION (AsyncSafetyMode(), step 2 of the proof).
			// Refused pages are NOT dropped and NOT lost: they go on the
			// deferred list, the caller fills them synchronously in this same
			// tick under the ordinary budget, and the page is as correct as it
			// would have been in mode 2. The temporary in-flight mark is the
			// only trick here -- it makes NextPageToFill step past this page
			// for the rest of the batch (and, for a demand page, drop its queue
			// entry) instead of returning it forever. FillDeferredPagesSync
			// clears every mark before it fills anything.
			if (!IsPageAsyncSafe(P.X, P.Y))
			{
				PagesInFlight.Add(PageKey(P.X, P.Y));
				OutDeferred.Add(P);
				++AsyncDeclinedUnpinned;
				++AsyncDeclinedUnpinnedLifetime;
				continue;
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
			// Nothing left to fill, OR nothing the admission test will let a
			// worker touch. Either way the remaining slots stay idle this tick;
			// whatever was refused is on OutDeferred and the caller fills it on
			// the game thread. The two cases are told apart in the log by
			// `declinedUnpinned=` -- zero is the first, non-zero the second.
			return;
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

	// THE DEMAND CAP'S TICK BOUNDARY, and a streaming tick is the right unit
	// because it is the unit the cost came in. The measured hitch is 7-25
	// chunks declining inside ONE DispatchJobs, and DispatchJobs runs once per
	// streaming tick immediately after this call -- so resetting here bounds
	// exactly the batch that produced the 241 ms and lets a page crossing
	// spread over consecutive ticks instead of landing in one frame. A
	// per-frame or per-call reset would not: SubmitGpuMeshJob is called many
	// times inside the batch this is bounding. Also stale-proof: if the
	// subsystem ever stops calling Tick, the counter simply stops being
	// refilled and the demand path stops, which fails toward today's behaviour.
	DemandPagesThisTick = 0;

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
		// LATCH WHAT KIND OF SAMPLER WE WERE HANDED, once, and SAY IT. A leg
		// that cannot tell which arm it got cannot be read: the same command
		// line produces a guarded run on a fine-tier world and an unguarded
		// (and unnecessary to guard) one on a coarse world, and the difference
		// decides whether `asyncPages=0` is a bug or the design.
		if (!bAsyncSafetyLatched)
		{
			bAsyncSafetyLatched = true;
			bAsyncSamplerCanLeak = SamplerCanLeakOnWorker(Tiles);
			UE_LOG(LogVoxelRasterAtlas, Log,
			       TEXT("[raster-atlas] mode 3 ARMED: safety=%d, sampler %s reach the fine tier's fatal ")
			       TEXT("gate from a worker (pitch=%d mm/px), ringRadius=%d -> async admits pages within ")
			       TEXT("Chebyshev %d tile(s) of the anchor's tile. %s"),
			       AsyncSafetyMode(),
			       bAsyncSamplerCanLeak ? TEXT("CAN") : TEXT("cannot"),
			       Tiles.pixelSizeMm(), FineRingRadiusTiles(), FineRingRadiusTiles() - 1,
			       (AsyncSafetyMode() == 0)
			           ? TEXT("SAFETY OFF: this is the prime-and-hope arm and it CAN kill an unattended run.")
			       : (!bAsyncSamplerCanLeak)
			           ? TEXT("Nothing to guard against on this sampler; every page is admissible.")
			       : (FineRingRadiusTiles() >= 1)
			           ? TEXT("Guarded by the pinned ring; pages outside it fill synchronously.")
			           : TEXT("RING RADIUS 0 ADMITS NOTHING -- this run will fill synchronously and measure ")
			             TEXT("mode 2. Pass -VoxelFineTileRingRadius=1 to actually measure mode 3."));
		}

		// THE BARRIER -- step 4 of the proof at AsyncSafetyMode(). The anchor's
		// coarse tile changed, so the one crossing of slack the admission test
		// left is now spent: every worker still running must finish before the
		// NEXT residency tick can evict anything it is reading. Because
		// TickResidencyAndEviction runs earlier in this same streaming tick,
		// draining here is early enough, and the eviction pass that moved the
		// ring has already been survived.
		const vxc::TileCoord AnchorTile =
			vxc::tileCoordForPixel(LastAnchorPxX, LastAnchorPxY, int64(vxc::kFineTileSize));
		if (bAsyncSamplerCanLeak && AsyncSafetyMode() != 0 && AnyAsyncSlotBusy() &&
		    (AnchorTile.x != InFlightAnchorTileX || AnchorTile.y != InFlightAnchorTileY))
		{
			const uint64 DrainT0 = FPlatformTime::Cycles64();
			DrainAsyncSlots();
			AsyncDrainCycles += FPlatformTime::Cycles64() - DrainT0;
			++AsyncDrains;
			++AsyncDrainsLifetime;
		}
		InFlightAnchorTileX = AnchorTile.x;
		InFlightAnchorTileY = AnchorTile.y;

		HarvestAsyncSlots();
		// Declared here rather than inside LaunchAsyncSlots so that the pages
		// the admission test refuses are filled INSIDE the GtFillCycles bracket
		// -- the safety path's game-thread cost has to land in the number the
		// registered disproof is written against, not beside it.
		TArray<FPagePt> Deferred;
		LaunchAsyncSlots(Tiles, LastAnchorPxX, LastAnchorPxY, Deferred);
		FillDeferredPagesSync(Tiles, Deferred);
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
	//
	// THE DEMAND-FILL FIELDS, appended before `win=` so the old-leg-grep rule
	// still holds (`win=` stays last) and every existing grep on served= /
	// inlineFallback= / fills= still lands unchanged.
	//
	// HOW TO READ THEM TOGETHER, because no one of them is a verdict:
	//   * `inlineFallback` is now the count of chunks that ACTUALLY sampled a
	//     ~5,800-pixel window inline -- a rescued decline has been taken back
	//     off it (see PrepareRequest). On a flight leg it must FALL toward 0
	//     after warmup. Not falling means the demand path is not engaging and
	//     the leg is NOT MEASURED: it is the control path wearing the fix.
	//   * `demandRescued` is what it fell BY, and it is the proof of traffic.
	//     0 across a whole flight leg, with `inlineFallback` still large, is
	//     the DEAD reading -- and it cannot be confused with the healthy
	//     steady state, where `inlineFallback` is 0 too and there was simply
	//     nothing to rescue.
	//   * `demandMs` is the GAME-THREAD bill this path adds to the submit
	//     path. It is the number that has to be SMALL where
	//     `Voxel gpu submit split`'s raster= was 241 ms. Large demandMs with
	//     inlineFallback falling means the cost was renamed, not removed.
	//   * `demandRetryFail` must be 0. Non-zero says the fill and the coverage
	//     check disagree about which pages a window covers.
	UE_LOG(LogVoxelRasterAtlas, Log,
	       TEXT("[raster-atlas] window: served=%llu inlineFallback=%llu fills=%llu ")
	       TEXT("(%.2f MiB, %.1f ms GT) resident=%u/%u pages gpuMiss=%s lifetimeMiss=%llu ")
	       TEXT("| demand: calls=%llu rescued=%llu (lifetime %llu) pages=%llu (lifetime %llu) ")
	       TEXT("outOfDisc=%llu (lifetime %llu)%s ")
	       TEXT("capHit=%llu (lifetime %llu) cap=%d/tick %.1f ms GT retryFail=%llu%s ")
	       TEXT("| prefetch: cap=%d/tick ticks=%llu filled=%llu (lifetime %llu) ")
	       TEXT("alreadyResident=%llu %.1f ms GT%s win=%.2fs"),
	       Served, DeclinedCold, PagesFilled,
	       double(BytesUploaded) / (1024.0 * 1024.0),
	       MsFromCycles(GtFillCycles),
	       ResidentPages, PagesDim * PagesDim,
	       bHaveMiss ? *FString::Printf(TEXT("%u"), Miss.Misses) : TEXT("PENDING"),
	       GpuMissLifetime,
	       DemandCalls, DemandRescued, DemandRescuedLifetime,
	       DemandPagesFilled, DemandPagesFilledLifetime,
	       // The partition of `pages` that decides which fix the crossing lump
	       // needs. It fails apart in BOTH directions on purpose -- neither
	       // reading is the one the arm was built expecting, so this cannot
	       // come out only one way.
	       DemandOutOfDisc, DemandOutOfDiscLifetime,
	       (DemandPagesFilled == 0)
	           ? TEXT("")
	       : (DemandOutOfDisc == DemandPagesFilled)
	           ? TEXT(" ALL-OUT-OF-DISC (every page this window's demand path filled was outside ")
	             TEXT("IsPageInCoverage -- the sweep and any coverage-filtered prefetch can NEVER ")
	             TEXT("pre-fill them at any scan width; the fix is the coverage RADIUS)")
	       : (DemandOutOfDisc == 0)
	           ? TEXT(" ALL-IN-DISC (every page was inside coverage -- the sweep could have reached ")
	             TEXT("them and did not; the fix is prefetch scan reach/timing)")
	       : TEXT(""),
	       DemandCapHits, DemandCapHitsLifetime, DemandPagesPerTick(),
	       MsFromCycles(DemandCycles), DemandRetryFailLifetime,
	       (DemandRetryFailLifetime > 0)
	           ? TEXT(" FAIL-RETRY-REFUSED (the window's pages were filled and the coverage ")
	             TEXT("check still declined -- fill and check disagree; this leg is invalid)")
	       : (DemandPagesPerTick() == 0)
	           ? TEXT(" DEMAND-PATH-OFF (control arm: -VoxelGpuRasterAtlasDemandPagesPerTick=0)")
	       : (DemandCalls == 0 && DeclinedCold > 0)
	           ? TEXT(" DEAD: chunks took the inline path and the demand path was never called")
	       : (DemandCalls > 0 && DemandRescued == 0 && DemandPagesFilled > 0)
	           ? TEXT(" FAIL: pages were filled and NOTHING was rescued")
	       : TEXT(""),
	       // The prefetch group (voxel.Stream.AtlasPrefetchAhead). The verdicts
	       // fail apart on purpose: OFF (never called -- the cvar is 0) can
	       // never be confused with ARMED-IDLE (called every tick, predicted
	       // page never differed -- a parked leg), and neither with the healthy
	       // armed steady state (ticks>0, crescent already resident). The A/B
	       // verdict does NOT live on this line: it is demand rescued=/capHit=
	       // collapsing above, and the submit split's maxRaster falling out of
	       // the tens of milliseconds on the armed leg.
	       PrefetchCapLast, PrefetchTicks, PrefetchFilled, PrefetchFilledLifetime,
	       PrefetchAlreadyResident, MsFromCycles(PrefetchCycles),
	       (PrefetchCapLast == 0)
	           ? TEXT(" OFF (voxel.Stream.AtlasPrefetchAhead=0)")
	       : (PrefetchTicks == 0)
	           ? TEXT(" ARMED-IDLE (predicted page never differed from the current one this window)")
	       : TEXT(""),
	       Elapsed);

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

	// WHICH SAFETY ARM THIS RUN IS ON, in words. The four states are printed
	// apart because they fail apart: "not latched" is a mode-3 run whose Tick
	// has never reached the async branch (a DEAD reading), "OFF-UNSAFE" is the
	// arm that can kill an unattended leg, "RING-0-ADMITS-NOTHING" is a leg
	// that thinks it is measuring mode 3 and is measuring mode 2, and
	// "PINNED-RING" is the guarded arm. None of them is "ok" without the
	// counters beside them.
	const TCHAR* SafetyState =
		(FillMode() < 3)              ? TEXT("n/a (mode<3)")
		: !bAsyncSafetyLatched        ? TEXT("NOT-LATCHED (mode 3 set but the async branch never ran)")
		: (AsyncSafetyMode() == 0)    ? TEXT("OFF-UNSAFE (prime-and-hope; can kill an unattended run)")
		: !bAsyncSamplerCanLeak       ? TEXT("n/a (this sampler has no worker path to the fatal gate)")
		: (FineRingRadiusTiles() >= 1) ? TEXT("PINNED-RING (guarded)")
		                              : TEXT("RING-0-ADMITS-NOTHING (this leg is measuring mode 2)");

	const double ClimatePixels = double(FMath::Max<uint64>(WindowCost.ClimatePixels, 1));
	UE_LOG(LogVoxelRasterAtlas, Log,
	       TEXT("[raster-atlas] fill: mode=%s | %s | perPage %.3f ms = elev %.3f (%.0f%%) ")
	       TEXT("+ climate %.3f (%.0f%%) + stage %.3f (%.0f%%) + resid %.3f (%.0f%%) ")
	       TEXT("| callsPerPage elev=%.0f climate=%.0f of %.0f px (dedup %s, %.0fx) ")
	       TEXT("| audit checked=%llu mismatch=%llu | async pages=%llu launches=%llu inFlight=%d ")
	       TEXT("prime=%llu | asyncSafety=%s ring=%d declinedUnpinned=%llu (lifetime %llu) ")
	       TEXT("syncFallback=%llu drains=%llu (lifetime %llu, %.2f ms GT) ")
	       TEXT("| disc %lld/%lld pages | lifetime fills=%llu win=%.2fs"),
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
	       SafetyState, FineRingRadiusTiles(),
	       AsyncDeclinedUnpinned, AsyncDeclinedUnpinnedLifetime, AsyncSyncFallbackPages,
	       AsyncDrains, AsyncDrainsLifetime, MsFromCycles(AsyncDrainCycles),
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
	// WINDOW counters reset; the two LIFETIME totals beside them deliberately
	// do not. A drain is rare enough (one per coarse-tile crossing) that a
	// per-window count is almost always 0, and a reader needs to be able to see
	// that the path has fired at all this run without grepping every window.
	AsyncDeclinedUnpinned = 0;
	AsyncSyncFallbackPages = 0;
	AsyncDrains = 0;
	AsyncDrainCycles = 0;
	// Same split as the async pair above: the WINDOW counts reset, the lifetime
	// totals beside them do not. `demandRetryFail` is lifetime-ONLY and has no
	// window twin on purpose -- it is a correctness latch, and a fault that
	// fired once four windows ago must still be visible on the line the reader
	// happens to open.
	DemandCalls = 0;
	DemandPagesFilled = 0;
	DemandOutOfDisc = 0;
	DemandRescued = 0;
	DemandCapHits = 0;
	DemandCycles = 0;
	PrefetchTicks = 0;
	PrefetchFilled = 0;
	PrefetchAlreadyResident = 0;
	PrefetchCycles = 0;
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

	// IS THIS THE DEMAND PATH'S RETRY? Taken and cleared here, so exactly one
	// PrepareRequest can ever be the retry for one FillWindowOnDemand -- the
	// caller makes them back to back on the same request (SubmitGpuMeshJob),
	// with nothing in between that could call in again.
	const bool bDemandRetry = bDemandRetryPending;
	bDemandRetryPending = false;

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
		if (bDemandRetry)
		{
			// HARD FAIL, and the one the counter exists for. FillWindowOnDemand
			// just filled every page CollectMissingWindowPages named for this
			// exact request, and the walk above -- the same rule, the same
			// fields, the same pitch -- still found a hole. The two cannot
			// disagree unless someone gave one of them a second spelling, which
			// is the D5 drift this class is built to make impossible. Counted
			// lifetime-only and printed on the window line; it must be 0.
			++DemandRetryFailLifetime;
		}
		++DeclinedCold;
		return false;
	}

	if (bDemandRetry)
	{
		// THE RESCUE, AND WHY IT DECREMENTS.
		//
		// `inlineFallback` (DeclinedCold) is documented -- in this header, in
		// VoxelWorldSubsystem's call site, and in every leg note written
		// against it -- as "chunks that took the inline FillRasterWindow path".
		// Before the demand path a decline and an inline fill were the same
		// event, so counting the decline was counting the fill. They are no
		// longer the same event: this chunk declined, was rescued, and is being
		// SERVED. Leaving the first check's decline on the counter would make
		// `inlineFallback` unable to fall to 0 even on a perfectly working
		// build, which would destroy the one reading rule the whole feature is
		// judged by. So the decline is taken back and the rescue is counted in
		// its own right, and the two are printed side by side so the trade is
		// visible rather than inferred.
		//
		// `PrepareCalls` is deliberately NOT adjusted: two coverage walks
		// really did run, and the us/call figure on the prepare line should say
		// so.
		if (DeclinedCold > 0)
		{
			--DeclinedCold;
		}
		++DemandRescued;
		++DemandRescuedLifetime;
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

void FVoxelRasterAtlasCpu::CollectMissingWindowPages(const FVoxelGpuRegionRequest& Req,
                                                     TArray<FPagePt>& Out) const
{
	// The SAME rule PrepareRequest and FillRasterWindow run -- coverage is
	// derived, never respelled. Lifted out of FillWindowNow's body when the
	// hot path (FillWindowOnDemand) needed the same page list, precisely so
	// there would not be two loops to drift apart: if this ever disagreed with
	// PrepareRequest's walk, the symptom would be `demandRetryFail`, which is
	// the counter that exists to catch exactly that.
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
				Out.Add(FPagePt{Px, Py});
			}
		}
	}
}

void FVoxelRasterAtlasCpu::FillWindowNow(vxc::ITileSampler& Tiles, const FVoxelGpuRegionRequest& Req)
{
	check(IsInGameThread());
	check(IsInitialized());
	// GATE CONTRACT UNCHANGED: every page of the window, no cap, no budget.
	// The per-tick cap lives in FillWindowOnDemand and NOT here, because the
	// verify harness (VoxelGpuVerify.cpp:2026 and :2117 -- until this change,
	// the only two callers this function had ever had) needs the fixture
	// COMPLETELY covered before it can assert anything about a tap.
	DemandPageScratch.Reset();
	CollectMissingWindowPages(Req, DemandPageScratch);
	for (const FPagePt& P : DemandPageScratch)
	{
		FillPage(Tiles, P.X, P.Y);
	}
	if (PendingDelta.PageMeta.Num() > 0)
	{
		GpuAtlas.EnqueueUpsert(MoveTemp(PendingDelta));
		PendingDelta = FVoxelRasterAtlasGpuDelta();
	}
}

bool FVoxelRasterAtlasCpu::FillWindowOnDemand(vxc::ITileSampler& Tiles,
                                              const FVoxelGpuRegionRequest& Req)
{
	check(IsInGameThread());
	check(IsInitialized());

	// The pitch fault disables the atlas for the session and every request must
	// take the inline path; filling pages we would never serve from would be
	// pure cost. PrepareRequest refuses first for the same reason.
	if (bLoggedPitchMismatch || DemandPagesPerTick() <= 0)
	{
		return false;
	}

	++DemandCalls;
	const uint64 T0 = FPlatformTime::Cycles64();
	// Counted in the caller's bracket ONLY -- see DemandCycles in the header
	// for why this is deliberately outside GtFillCycles.
	ON_SCOPE_EXIT { DemandCycles += FPlatformTime::Cycles64() - T0; };

	DemandPageScratch.Reset();
	CollectMissingWindowPages(Req, DemandPageScratch);
	if (DemandPageScratch.Num() == 0)
	{
		// The coverage check declined two lines ago over this same rule, so the
		// only way to get here is that the pages it was missing became resident
		// in between -- which nothing on the game thread does between those two
		// calls. Rare by construction, and NOT an error: returning false sends
		// the chunk to the inline path exactly as today. `demandCalls` moving
		// while `demandPages` does not is the reading that says this is
		// happening a lot, and it would mean the two walks disagree.
		return false;
	}

	// THE CAP, ALL-OR-NOTHING. See DemandPagesPerTick() above: a partial fill
	// would leave the window uncovered and the chunk would pay the pages AND
	// the inline window, which is strictly worse than paying the inline window
	// alone. The pages are already in the demand queue -- PrepareRequest put
	// them there when it declined, before this was called -- so refusing here
	// costs nothing but a tick of latency.
	const int32 Remaining = DemandPagesPerTick() - DemandPagesThisTick;
	if (DemandPageScratch.Num() > Remaining)
	{
		++DemandCapHits;
		++DemandCapHitsLifetime;
		return false;
	}

	for (const FPagePt& P : DemandPageScratch)
	{
		// CLASSIFIED AT THE FILL, THROUGH THE ONE COVERAGE RULE. Was this page
		// ever going to be pre-filled by anything? The sweep and the predictive
		// prefetch both admit only pages IsPageInCoverage accepts, so a page
		// that fails this test here is one neither of them can reach at any
		// scan width -- see demandOutOfDisc in the header for the two fixes
		// this tells apart. Not derived later from a page list: the anchor a
		// page was judged against has to be the anchor it faulted under.
		//
		// LastAnchorPx is THIS streaming tick's anchor: Tick() stamps it and
		// the subsystem calls Tick() earlier in the same tick than DispatchJobs
		// -- the same ordering the delta flush already rests on. One extra
		// IsPageInCoverage per demanded page (single digits per crossing).
		if (!IsPageInCoverage(P.X, P.Y, LastAnchorPxX, LastAnchorPxY))
		{
			++DemandOutOfDisc;
			++DemandOutOfDiscLifetime;
		}
		FillPage(Tiles, P.X, P.Y);
	}
	DemandPagesThisTick += DemandPageScratch.Num();
	DemandPagesFilled += uint64(DemandPageScratch.Num());
	DemandPagesFilledLifetime += uint64(DemandPageScratch.Num());

	// FLUSH NOW, not at the next Tick, and the ordering argument is the one
	// PrepareRequest's host mirror already rests on: this runs inside
	// SubmitGpuMeshJob, which FVoxelWorldImpl runs from DispatchJobs BEFORE
	// GpuMeshJobs->Tick() enqueues any dispatch command. Render commands run in
	// enqueue order, so the upsert lands on the GPU ahead of every dispatch
	// this tick admits against the tags it just flipped.
	if (PendingDelta.PageMeta.Num() > 0)
	{
		GpuAtlas.EnqueueUpsert(MoveTemp(PendingDelta));
		PendingDelta = FVoxelRasterAtlasGpuDelta();
	}

	// Arms the retry's accounting; the next PrepareRequest takes and clears it.
	bDemandRetryPending = true;
	return true;
}

// THE PREDICTIVE COLUMN PREFETCH -- the WHY, the geometry bound and the
// failing readings are on the declaration in the header. Mechanically this is
// the demand path's fill by a different trigger: same FillPage, same mirror,
// same delta -- only WHEN differs (before the crossing instead of during it),
// which is the entire point. It stages into PendingDelta and deliberately does
// NOT flush: the caller runs it immediately BEFORE Tick() in the same
// streaming tick, so the pages ride Tick's flush and land on the GPU with the
// ordering guarantee every other fill already rests on.
void FVoxelRasterAtlasCpu::PrefetchAhead(vxc::ITileSampler& Tiles, int64 AnchorXMm,
                                         int64 AnchorYMm, int64 PredXMm, int64 PredYMm,
                                         int32 MaxPages)
{
	check(IsInGameThread());
	if (!IsInitialized() || MaxPages <= 0)
	{
		return;
	}
	PrefetchCapLast = MaxPages; // 0-vs-armed on the window line
	// Same session-fault rule as every other fill entry: after a pitch
	// mismatch the atlas serves nothing, so filling would be pure cost. Tick
	// (which runs right after this) owns the loud one-time log.
	if (bLoggedPitchMismatch || Tiles.pixelSizeMm() != PixelSizeMm)
	{
		return;
	}

	const int64 CurPxX = vxc::floorDiv(AnchorXMm, PixelSizeMm);
	const int64 CurPxY = vxc::floorDiv(AnchorYMm, PixelSizeMm);
	const int64 PredPxX = vxc::floorDiv(PredXMm, PixelSizeMm);
	const int64 PredPxY = vxc::floorDiv(PredYMm, PixelSizeMm);
	// The cheap idle test: while the predicted anchor sits in the SAME page as
	// the real one, the two coverage discs admit identical page sets and the
	// crescent below is empty by construction -- so a parked or slow-moving
	// leg pays two floorDivs and this compare per tick, nothing else.
	if (vxc::floorDiv(PredPxX, int64(kPagePx)) == vxc::floorDiv(CurPxX, int64(kPagePx)) &&
	    vxc::floorDiv(PredPxY, int64(kPagePx)) == vxc::floorDiv(CurPxY, int64(kPagePx)))
	{
		return;
	}

	++PrefetchTicks;
	const uint64 T0 = FPlatformTime::Cycles64();
	// Its own bracket, NOT GtFillCycles: that bracket is Tick's sweep and only
	// that (the registered disproof for fill mode 3 is written against it), and
	// a new cost hiding inside the number it is meant to relieve could never
	// show that it failed. Printed as `prefetch ... ms GT` on the window line.
	ON_SCOPE_EXIT { PrefetchCycles += FPlatformTime::Cycles64() - T0; };

	// Nearest-first Chebyshev rings around the PREDICTED anchor's page, rim
	// rings only: with the lead clamped under one page (60 m vs 240 m), a page
	// inside the predicted disc but outside the current one is within ~2 rings
	// of the rim (dist >= CoverageRadius - lead), so starting at RadiusPages-3
	// bounds the scan to a few ring perimeters (~hundreds of probes) instead
	// of the full 39x39 square. Each probe is two IsPageInCoverage tests and a
	// mirror lookup -- the same reads the sweep makes.
	const int64 CenterPageX = vxc::floorDiv(PredPxX, int64(kPagePx));
	const int64 CenterPageY = vxc::floorDiv(PredPxY, int64(kPagePx));
	int32 Filled = 0;
	for (int64 R = FMath::Max<int64>(0, RadiusPages - 3); R <= RadiusPages; ++R)
	{
		const int64 X0 = CenterPageX - R;
		const int64 X1 = CenterPageX + R;
		const int64 Y0 = CenterPageY - R;
		const int64 Y1 = CenterPageY + R;
		for (int64 Py = Y0; Py <= Y1; ++Py)
		{
			// Ring perimeter only, exactly as NextPageToFill walks it.
			const int64 StepX = (Py == Y0 || Py == Y1) ? 1 : (X1 - X0 > 0 ? X1 - X0 : 1);
			for (int64 Px = X0; Px <= X1; Px += StepX)
			{
				// THE CRESCENT, both tests through the ONE coverage rule:
				// needed once the camera arrives, and not already the sweep's
				// business today. A page in CURRENT coverage is skipped even if
				// missing -- the sweep and the demand queue own it, and racing
				// them from here would just reorder the same work.
				//
				// AND THIS FIRST TEST IS THE SUSPECTED CEILING, not the
				// rim-ring start bound dd4ee9e blamed. A page a chunk's window
				// can tap is NOT necessarily a page IsPageInCoverage admits:
				// admission takes a chunk CENTRE to AdmitOuterUU and the window
				// reaches past that centre again, while the disc is Outer plus
				// Init's margin alone. Any such page is refused here at every
				// scan width. UNCONFIRMED until a leg reads `outOfDisc=` on the
				// window line -- that counter exists to settle it and can come
				// out either way.
				if (!IsPageInCoverage(Px, Py, PredPxX, PredPxY))
				{
					continue;
				}
				if (IsPageInCoverage(Px, Py, CurPxX, CurPxY))
				{
					continue;
				}
				if (IsPageResident(Px, Py) || PagesInFlight.Contains(PageKey(Px, Py)))
				{
					++PrefetchAlreadyResident;
					continue;
				}
				FillPage(Tiles, Px, Py);
				++PrefetchFilled;
				++PrefetchFilledLifetime;
				if (++Filled >= MaxPages)
				{
					// The count IS the budget; the rest of the column is later
					// ticks' work, by design.
					return;
				}
			}
		}
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
