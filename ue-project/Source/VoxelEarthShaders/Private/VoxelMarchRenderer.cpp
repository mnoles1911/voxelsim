// VoxelMarchRenderer.cpp -- RDG plumbing for the marcher that draws.
//
// VoxelMarchRenderer.h owns the SEAM argument (which hooks, why those, what the
// base pass cannot do) and VoxelMarch.usf owns the GBuffer channel table and the
// velocity arithmetic. This file is the wiring between them, following
// VoxelFluidRender.cpp's shape: FGlobalShader classes, parameter structs,
// IMPLEMENT_GLOBAL_SHADER off the virtual path, and the project's raster passes
// built by hand rather than through a mesh pass.

#include "VoxelMarchRenderer.h"

#include <atomic>

#include "VoxelBrickPool.h"       // the P3-B1 traversal source
#include "VoxelMarchChunkIndex.h"
#include "VoxelMarchChunkIndex.h" // and the GPU lookup that makes it walkable
#include "VoxelHeightPyramid.h"  // the terrain-height upper bound the marcher skips air with
#include "VoxelGIVolume.h"       // the marcher now samples voxel GI
#include "SystemTextures.h"       // GetDefaultBuffer, for the emit pass's index fallback
#include "VoxelFluidOccupancy.h" // the P3 traversal source; see the header, section 5

#include "CommonRenderResources.h" // GEmptyVertexDeclaration
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/World.h"
#include "FXRenderingUtils.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h" // the hit-tile count ring
#include "GBufferInfo.h"  // FGBufferBindings -- which engine slot holds which GBuffer
#include "RenderUtils.h" // IsStaticLightingAllowed -- the GBufferE / velocity slot predicate
#include "SceneRenderTargetParameters.h" // ESceneTextureSetupMode
#include "SceneTexturesConfig.h"         // FSceneTextureUniformParameters (ENGINE_API, public)
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "VoxelRenderFrame.h" // the render-frame split: anchors A and B live in this file
#include "VoxelMarchBound.h" // voxel.March.Bound's producer (Stage 0b) -- VoxelMarchBoundProduce

DEFINE_LOG_CATEGORY_STATIC(LogVoxelMarch, Log, All);

// Two `stat GPU` lines, not one, for the same reason the two timing rings are
// separate: the classify+march half and the SV_Depth emit half sit in different
// places in the frame and answer different questions, and a combined scope
// makes the HTILE budget uncombinable from the march cost.
DECLARE_GPU_STAT_NAMED(VoxelMarch, TEXT("VoxelMarch"));
DECLARE_GPU_STAT_NAMED(VoxelMarchEmit, TEXT("VoxelMarchEmit"));

// Macro, not a const TCHAR*: IMPLEMENT_GLOBAL_SHADER stringizes its path
// argument (same note as VoxelFluidSim.cpp:21).
#define VOXEL_MARCH_USF "/VoxelEarth/VoxelMarch.usf"

// One tile is one thread group is one 8x8 pixel block. The three have to agree
// -- the march's per-group hit reduction is what fills the emit's tile list, so
// a group that is not exactly a tile would need a second atomic per pixel.
static constexpr int32 kVoxelMarchTileSize = 8;

namespace
{
	// ---- the phase gate ---------------------------------------------------

	TAutoConsoleVariable<int32> CVarVoxelMarch(
		TEXT("voxel.March"), 1,
		TEXT("P3 of docs/ray-marching-plan-2026-08-19.md: draw terrain by ray-marching the voxel ")
		TEXT("volume into SceneDepth + the GBuffer, so the engine's lighting, shadows, SSAO, TSR ")
		TEXT("and Lumen screen traces all work on it.\n")
		TEXT("  0 = OFF, and off costs NOTHING: the view extension declines every frame, no pass ")
		TEXT("is added, no texture is allocated, no query is issued. The frame is byte-identical ")
		TEXT("to a build without this feature. THIS IS THE CONTROL AND IT IS THE DEFAULT.\n")
		TEXT("  1 = the marcher writes the REAL scene depth, stencil, GBufferA..E, SceneColor and ")
		TEXT("Velocity.\n")
		TEXT("  2 = BOTH: the raster path keeps the real targets and the marcher writes a full ")
		TEXT("SCRATCH copy of them, so what is on screen is the control image while every marcher ")
		TEXT("pass still runs and still costs what it costs. This is the mode the depth and image ")
		TEXT("gates run in -- one binary, one session, one leg.\n")
		TEXT("IT DOES NOT SUPPRESS THE QUAD PATH. That is P4. Suppression is ")
		TEXT("voxel.Stream.GPUCullDebugDrawNothing 1, which already exists. RUNNING MODE 1 WITHOUT ")
		TEXT("IT IS A DIAGNOSTIC COMBINATION ONLY: the quads are then in the depth prepass at the ")
		TEXT("same depth as the marched surface, DepthNearOrEqual accepts about half the pixels, ")
		TEXT("and the result is per-pixel noise that looks exactly like a broken marcher and is ")
		TEXT("not one.\n")
		TEXT("NEEDS voxel.Fluid.Enable 1 until the brick pool lands (P2): the traversal source is ")
		TEXT("still the fluid occupancy volume, so with the fluid off there is nothing to march ")
		TEXT("and the frame is declined and counted as such."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchSource(
		TEXT("voxel.March.Source"), 1,
		TEXT("WHICH VOLUME THE MARCHER WALKS. 0 = FVoxelFluidOccupancy, the Phase 0 stand-in: ")
		TEXT("512^3, 51.2 m, ONE level, one bit per voxel and NO PER-VOXEL MATERIAL. 1 = ")
		TEXT("FVoxelBrickPool, the real volume -- six ring levels, per-brick descriptors, ")
		TEXT("collapsed bricks, palette-indexed materials.\n")
		TEXT("P3-B1 MARCHES LEVEL 0 ONLY on source 1: no cone LOD, no ring transitions, no mip ")
		TEXT("pyramid, and no empty-space skipping beyond the L1 brick-mask test that costs ")
		TEXT("nothing. R0 spans 0-128 m, which comfortably contains the occupancy volume's ")
		TEXT("51.2 m, so the two sources can be compared on the same ground at the same pose.\n")
		TEXT("THE DELIVERABLE IS marchMs ON SOURCE 1 AGAINST THE 5.06-5.21 ms CLUSTER SOURCE 0 ")
		TEXT("PRODUCED. That difference is the per-step cost of five indirections, and the cost ")
		TEXT("model `0.13 + steps * 5.9 us` is DEAD until it is measured -- it was taken on a ")
		TEXT("1-bit flat volume where a solidity test is three integer ops and one load.\n")
		TEXT("SOURCE 1 STILL NEEDS voxel.Fluid.Enable 1 in this step. Not because it reads the ")
		TEXT("occupancy volume -- it does not -- but because the marcher's game-thread hookup ")
		TEXT("rides the fluid subsystem's tick, and because running BOTH arms with the solver on ")
		TEXT("is what keeps the A/B like-for-like. Decoupling it is P3-B2 work and would change ")
		TEXT("the conditions of the very comparison this step exists to make."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchSkipLevels(
		TEXT("voxel.March.SkipLevels"), 2,
		TEXT("EMPTY-SPACE SKIPPING, and the whole performance case. 0 = flat: every voxel ")
		TEXT("stepped and tested -- THE CONTROL, and the same walk that produced the measured ")
		TEXT("+18.3%% indirection cost. 1 = skip an empty 8^3 brick on one bit test against a ")
		TEXT("register (512 voxels, zero memory traffic). 2 = skip a non-resident or all-air ")
		TEXT("32^3 chunk on one index dword as well (32,768 voxels).\n")
		TEXT("A SHADER PERMUTATION, NOT A UNIFORM. A runtime branch leaves the other arm's loads ")
		TEXT("in the binary, so the control pays traffic it never uses and the ratio comes out ")
		TEXT("wrong in the flattering direction -- VoxelMarchSpike.usf recorded this and it ")
		TEXT("should not need rediscovering.\n")
		TEXT("THE RATIO IS MEASURED BY voxel.March.VerifySource, not by comparing two legs: it ")
		TEXT("runs the flat and hierarchical walks over the SAME rays in the SAME frame and ")
		TEXT("reports stepsFlat/stepsHier plus the disagreement count that gates it. 9.19x came ")
		TEXT("from a two-level mip over a FLAT volume and does not transfer to chunks->bricks->cells."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchRings(
		TEXT("voxel.March.Rings"), 1,
		TEXT("The ring cascade (P3-B2b-1). 0 = one level over one interval, which is every leg ")
		TEXT("measured before 2026-08-20 and is THE CONTROL. 1 = two rings, L0 covering 0-128 m ")
		TEXT("and L1 covering 128-256 m, no overlap.\n")
		TEXT("IT IS A SHADER PERMUTATION, not a runtime branch, for the same reason the source ")
		TEXT("is: a runtime branch leaves both paths in the binary and silently re-bases whichever ")
		TEXT("arm is being measured. With rings off the generated code is byte-identical to the ")
		TEXT("level-0 baseline, which is what makes rings-off a control rather than a near-one.\n")
		TEXT("OVERLAP IS ZERO AND STAYS ZERO. Padding every ring's admit band measured +9.2%% ")
		TEXT("resident chunks, p50 14.9 -> 17.3 ms, chunks/s 968 -> 672 and hitches 1 -> 47. With ")
		TEXT("overlap 0 the seam is VISIBLE and ATTRIBUTABLE: a silhouette pop is either the ")
		TEXT("missing overlap or the traversal, and the ring-discordant column separates them. ")
		TEXT("The marcher has no mesh, so the crack the overlap exists to hide may not exist for ")
		TEXT("it at all -- a question to test, not to assume either way.\n")
		TEXT("SOURCE 1 ONLY. There is one occupancy volume and it has no levels."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchFallthrough(
		TEXT("voxel.March.Fallthrough"), 1,
		TEXT("DEFAULT 1 SINCE 2026-08-23, ON THE OWNER'S DIRECT VISUAL VERDICT, AND IT ")
		TEXT("OVERRULED THREE OF MY MEASUREMENTS. Toggled live in PIE it closed the black ")
		TEXT("gaps at every LOD boundary the moment it went on, and re-opened them the ")
		TEXT("moment it went off; flying, the holes became a wait for finer detail to ")
		TEXT("arrive over coarse terrain instead of a wait for anything at all. ")
		TEXT("THE INSTRUMENT SAID THE OPPOSITE: three matched legs put voxel.March.HoleStats' ")
		TEXT("`uncovered` HIGHER with this on (3.99%% -> 4.56%%), which is backwards -- a ray ")
		TEXT("that would have missed now hits at a coarser level, so uncovered should FALL. ")
		TEXT("Until that is explained, `uncovered` is NOT a trustworthy hole gate and the ")
		TEXT("screenshots outrank it. See docs/gpu-streaming-architecture.md 7. ")
		TEXT("FINE -> COARSE FALLTHROUGH (Phase 1, the no-hole invariant): how many coarser ")
		TEXT("levels a ring segment may retry after a miss that crossed a NON-RESIDENT chunk, so ")
		TEXT("a missing level-0 chunk renders as level-1 detail instead of a hole. 0 = off, the ")
		TEXT("byte-identical control -- and NOT the default: THIS SHIPS AT 1 since 2026-08-23, ")
		TEXT("because the owner reports black arc holes at LOD boundaries without it. It is a ")
		TEXT("SHADER PERMUTATION for the ")
		TEXT("same reason rings are. 1 is the intended arm (worst case two walks per segment); 2 ")
		TEXT("is the measurable step beyond it. Clamped to 0..2.\n")
		TEXT("THE GATE IS RESIDENCY, NOT VALIDITY: a resident-but-empty chunk is real air and ")
		TEXT("never falls through -- unconditional fallthrough would plug cave mouths, doorways ")
		TEXT("and overhangs with coarse rock, a visible regression no counter catches. Sky rays ")
		TEXT("are NOT protected by the gate (sky-band-trimmed chunks are legitimately ")
		TEXT("non-resident), so each depth step is paid on them; that is the cost ceiling.\n")
		TEXT("USELESS WITHOUT HIERARCHICAL COVERAGE on the game side: the coarse levels must ")
		TEXT("actually COVER the ground inside them for a coarser retry to find anything. With ")
		TEXT("coverage off this fires and misses, burning the extra walks for nothing. Coverage ")
		TEXT("is ON BY DEFAULT since 2026-08-23 and its switch is the NEGATIVE ")
		TEXT("-VoxelNoHierarchicalCoverage (VoxelWorldSubsystem.cpp HierarchicalCoverageEnabled). ")
		TEXT("Do NOT read a command line for -VoxelHierarchicalCoverage to decide whether this ")
		TEXT("arm is useful: nothing passes it any more. ")
		TEXT("Rings + source 1 only."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchHoleStats(
		TEXT("voxel.March.HoleStats"), 0,
		TEXT("THE HOLE METRIC. The owner's complaint is holes while flying at 30 m/s and this ")
		TEXT("is its instrument -- until it existed the only reading was a screenshot. Counts, ")
		TEXT("per frame on the SHIPPING kernel: rays, hits, `substituted` (a hit from a level ")
		TEXT("COARSER than the segment that owns that ground -- the no-hole invariant visibly ")
		TEXT("doing its job) and `uncovered` (no hit at any level, AND the ray crossed a chunk ")
		TEXT("that is ABSENT -- not resident-and-empty -- AND it points below the horizon: the ")
		TEXT("real hole count). Printed on the 5 s LogVoxelPerf line with rays alongside.\n")
		TEXT("NOT the naive metric, deliberately: 'ray exited the cascade with no hit' counts ")
		TEXT("the SKY -- every sky ray satisfies it and the sky-band trim keeps non-resident ")
		TEXT("space above every ridge, so it can never read zero. Both counters here move BOTH ")
		TEXT("ways: -VoxelMaxRingLevel=0 makes `uncovered` large; a settled stationary world ")
		TEXT("reads it near zero; `substituted` rises in flight with voxel.March.Fallthrough>0 ")
		TEXT("and falls to zero at rest or with fallthrough 0.\n")
		TEXT("A SHADER PERMUTATION, default 0, and off is FREE: no UAV is created or bound, no ")
		TEXT("groupshared word exists, no atomic runs -- the off arm is the byte-identical ")
		TEXT("control, same rule as rings and fallthrough. Rings + source 1 only.\n")
		TEXT("2 = THE BREAKDOWN (2026-08-23, the dark-arcs instrument): every uncovered ray also ")
		TEXT("reports WHICH ring level owned the ground at its first absent crossing (histogram ")
		TEXT("over the 6 levels, not a mean) and WHY that chunk was absent -- never admitted / ")
		TEXT("admitted-pending / evicted / unattributed -- read from annotation bits the CPU ")
		TEXT("streamer writes into NON-resident index cells (see VoxelMarchIndexCell.ush). ")
		TEXT("PROVING RUNS: -VoxelMaxRingLevel=0 must push the reason mass into NEVER-ADMITTED ")
		TEXT("at levels 1-5 (streaming never admits them); hierarchical coverage inflates ")
		TEXT("EVICTED (32,923 pool evictions measured -- coverage is now ON by default, so the ")
		TEXT("control leg for that comparison is -VoxelNoHierarchicalCoverage); hovering must ")
		TEXT("drain PENDING to ")
		TEXT("~0 alongside the pending queues. A large UNATTRIBUTED bucket, or a printed ")
		TEXT("attributed-vs-uncovered shortfall, indicts the instrument itself and is printed ")
		TEXT("rather than folded away. Level 1 leaves the breakdown words unwritten and the log ")
		TEXT("says NOT MEASURED for them -- never zero. INCOMPATIBLE with ")
		TEXT("voxel.March.IndexGpuResident 1: the GPU publish kernel clears cells to literal 0, ")
		TEXT("so the annotations are not written there and the whole breakdown reads ")
		TEXT("never-admitted; the writer disarms itself and the perf line says so."),
		ECVF_RenderThreadSafe);

	// ---- THE RESIDENT-Z BOUND -------------------------------------------
	//
	// Measured gap, four matched static legs, real ProfileGPU, one spawn, only
	// camera PITCH differing:
	//
	//     -90 down   VoxelMarch.March 1.108 ms   frame 7.45 ms
	//     -20        3.656 ms                    8.76 ms
	//       0        4.448 ms                    9.08 ms
	//     +30 sky    5.638 ms                    9.55 ms
	//
	// 5.1x and a 4.5 ms swing on ONE GPU pass, tracking EMPTY SPACE CROSSED
	// rather than geometry hit. At the horizon this pass is 49% of the frame.
	// The cause is structural, not micro: above the chunk grid there is no
	// acceleration structure and NO Z BOUND AT ALL, so a ray within ~2.2
	// degrees of vertical walks the full 4,198 m FrameExtentUU half-extent at
	// level 0 -- about 1,310 chunk steps -- hits the 512-chunk cap, returns
	// TERM_CHUNK_CAP and produces nothing. voxel.March.StepBudget cannot stop
	// it: its only consumer sits INSIDE the brick loop and an empty ray never
	// reaches a brick.
	TAutoConsoleVariable<int32> CVarVoxelMarchZCut(
		TEXT("voxel.March.ZCut"), 0,
		TEXT("THE RESIDENT-Z BOUND. 0 = off, THE CONTROL, and the default. 1 = before walking a ")
		TEXT("ring segment at level L, intersect the segment's t-interval with the Z SLAB that ")
		TEXT("CONTAINS every chunk level L holds, and hand the walk the intersection instead. An ")
		TEXT("empty intersection skips the walk outright.\n")
		TEXT("WHAT IT IS FOR: the ring walk is bounded horizontally (ring radii, corrected to ")
		TEXT("CYLINDERS in 2026-08-20) and not bounded vertically at all, while the streamed set ")
		TEXT("is a thin shell around the surface -- about 4.4 chunks per column at level 0. A ")
		TEXT("near-vertical ray therefore spends its entire budget stepping through air nothing ")
		TEXT("holds, one scattered load into a ~67 MB buffer per 3.2 m.\n")
		TEXT("A UNIFORM AND NOT A PERMUTATION, which is a departure from voxel.March.Rings / ")
		TEXT("Fallthrough / HoleStats and is argued rather than assumed: the doctrine those three ")
		TEXT("follow is that a runtime branch leaves the OTHER ARM'S LOADS in the binary and ")
		TEXT("re-bases the control. This arm adds no load and no memory traffic of any kind -- a ")
		TEXT("handful of ALU per SEGMENT (at most 7 per ray) against hundreds of chunk steps -- so ")
		TEXT("there is no traffic for an off-arm to carry. Being a uniform is also what lets one ")
		TEXT("build serve both arms of a pitch sweep, which is how this will be measured.\n")
		TEXT("IT CANNOT MAKE A HOLE, and that is the property that had to be built rather than ")
		TEXT("hoped for. The slab is the CUMULATIVE union of every chunk Z ever admitted at that ")
		TEXT("level (FVoxelMarchChunkIndex::GetResidentChunkZBound), widened further by ")
		TEXT("voxel.March.ZCutPadChunks; removals never narrow it. So the bound is a strict ")
		TEXT("SUPERSET of the resident set, every chunk the cut removes is provably NOT RESIDENT, ")
		TEXT("and a level holding nothing is refused a bound entirely rather than given an empty ")
		TEXT("one. Its failure mode over a long flight is that the union widens until the cut ")
		TEXT("removes nothing -- LOST BENEFIT, never a hole.\n")
		TEXT("FALLTHROUGH SEMANTICS ARE PRESERVED EXACTLY. bCrossedAbsentChunk is set whenever ")
		TEXT("the cut removes any part of an interval, because a full walk over that part would ")
		TEXT("have set it; and the pad guarantees every removed chunk is at least TWO chunks in Z ")
		TEXT("from anything resident, so it cannot be face-adjacent to a resident chunk and ")
		TEXT("bCrossedShellAbsent is left untouched -- which is exactly what the full walk would ")
		TEXT("have left it. Both fallthrough gates therefore see the same value they saw before.\n")
		TEXT("PROVE IT ENGAGED BEFORE BELIEVING A TIMING: voxel.March.Stats prints the arm state, ")
		TEXT("the per-level bound actually uploaded, and -- with voxel.March.HoleStats on -- ")
		TEXT("consulted / skipped / clipped, which partition every decision the bound was asked ")
		TEXT("to make. consulted == 0 with this at 1 means ")
		TEXT("the arm is ARMED AND INERT and no timing taken on that leg means anything.\n")
		TEXT("THOSE COUNTERS PROVE ENGAGEMENT AND DO NOT ESTIMATE THE SAVING. Iterations and ")
		TEXT("wall time move in opposite directions often enough to have been published three ")
		TEXT("times over -- VoxelRT measured iterations -60%% against traversal time +10-15%%, ")
		TEXT("and Aila & Laine record that ~20%% slower code gives the same measured perf. ")
		TEXT("THE SAVING IS `VoxelMarch.March` FROM ProfileGPU AND NOTHING ELSE. A leg where ")
		TEXT("skipped is large and that number does not move is a REAL NULL RESULT, not a ")
		TEXT("broken counter.\n")
		TEXT("WHAT WOULD REFUTE IT: any black arc or missing ground that appears with this at 1 ")
		TEXT("and is absent at 0 on the same spawn. That is a hole, it outranks every millisecond ")
		TEXT("here, and the correct response is to turn this off, not to widen the pad."),
		ECVF_RenderThreadSafe);

	// ---- THE PER-RAY RESIDENT-EXTENT BOUND (Stage 0b) -------------------
	//
	// THE GATE THAT AUTHORISED IT, measured by the Stage 0a census on the
	// shipping kernel at three pitches: removableUp = 91.08% of chunk-loop
	// iterations at the horizon (the pre-registered gate was 40%), 95.48% at
	// sky, removableDown = 39.5% looking down, capRays = 0.0000% everywhere.
	// The removable mass is predominantly PRE-hit iterations on up/horizon
	// rays -- whole ring segments crossing ZERO resident chunks at their
	// level -- so the arm's PRIMARY payoff is skipping such segments whole
	// (boundSegmentsSkipped); the end-trims are secondary. The Z slab could
	// not reach this mass (it skipped 0.00% at the horizon: a horizontal ray
	// never leaves a 118 m slab); this bound is PER PIXEL AND PER RAY.
	TAutoConsoleVariable<int32> CVarVoxelMarchBound(
		TEXT("voxel.March.Bound"), 0,
		TEXT("THE PER-RAY RESIDENT-EXTENT BOUND. 0 = off, THE CONTROL, and the default. 1 = ")
		TEXT("rasterise one cube per chunk record live in the brick pool's own table into a ")
		TEXT("per-ring-level Texture2DArray under min-blending, giving every pixel the interval ")
		TEXT("of ray-t its ray spends inside resident level-L space; the ring walk then clamps ")
		TEXT("each segment's [WalkIn, WalkOut] to that interval, and a segment whose interval ")
		TEXT("is EMPTY is skipped outright -- the primary payoff, per the Stage 0a census ")
		TEXT("(removableUp 91.08%% at the horizon, mass in PRE-hit ring segments).\n")
		TEXT("A PERMUTATION, NOT A UNIFORM -- BlockSkip's argument, not ZCut's: the consumer ")
		TEXT("adds up to seven texture loads per ray and a runtime branch would leave that ")
		TEXT("traffic in the control's binary. 0 is byte-identical.\n")
		TEXT("IT CANNOT MAKE A HOLE, and the argument is validation, not residency-as-air: a ")
		TEXT("hit requires Chunk.bValid, which VoxelMarchLookupChunk grants only through a ")
		TEXT("record in the SAME chunk table the producer rasterises (a superset of it -- no ")
		TEXT("anySolid filter, no index join), so removed space is UNREACHABLE BY A HIT, ")
		TEXT("whatever the terrain truth. bCrossedAbsentChunk is folded for every removed ")
		TEXT("interval, so the fallthrough ladder sees exactly the control's value and coarse ")
		TEXT("stand-ins still fill unstreamed ground. The clamp is biased outward one full ")
		TEXT("chunk per end at the walk's level (the ZCutBiasUU constant and the Danskin & ")
		TEXT("Hanrahan argument, restated at the clamp).\n")
		TEXT("REFUSED PAIRINGS, at compile and here: rings off (no socket), voxel.March.SkyLadder ")
		TEXT("(its gate reads bWalkTruncated, which this bound eliminates -- the ladder would ")
		TEXT("close on rays that retry today; SkyLadder wins and this arm is forced 0), and ")
		TEXT("voxel.March.HalfRes (the sample lattice is not the raster lattice; a bound for a ")
		TEXT("different ray is no bound).\n")
		TEXT("PROVE IT ENGAGED BEFORE BELIEVING A TIMING: with voxel.March.HoleStats on, the ")
		TEXT("bound engagement line prints consulted / segmentsSkipped / walkInRaised / ")
		TEXT("walkOutLowered plus the producer's own boundMs bracket. consulted == 0 with this ")
		TEXT("at 1 is the arm ARMED AND INERT and no timing on that leg means anything. Those ")
		TEXT("counters prove engagement and do NOT estimate the saving -- THE SAVING IS ")
		TEXT("`VoxelMarch.March` FROM ProfileGPU MINUS THE BOUND BRACKET, AND NOTHING ELSE.\n")
		TEXT("WHAT WOULD REFUTE IT: any black arc or missing ground at 1 that is absent at 0 on ")
		TEXT("the same spawn. A hole outranks every millisecond here; turn the arm off, never ")
		TEXT("widen the bias."),
		ECVF_RenderThreadSafe);

	// =====================================================================
	// THE COARSE OCCUPANCY LEVEL (voxel.March.BlockSkip)
	// =====================================================================
	//
	// THE GAP THIS CLOSES, and it is the one the resident-Z bound could not.
	// The chunk index is 99.7% EMPTY -- 50,052 resident chunks
	// (perLevel 6671/7070/7082/7226/7112/7275/7616) in a 128^3 x 8-slot,
	// 16,777,216-cell, 64 MiB grid -- and the walk pays one scattered 4-byte
	// load into that 64 MiB for every 3.2 m of nothing at level 0. The marcher
	// is 54% of the GPU frame and VoxelMarch.March swings 1.108 ms looking
	// straight down to 5.638 ms at sky, 5.1x, tracking EMPTY SPACE CROSSED.
	//
	// WHY NOT THE Z SLAB, WHICH IS ALREADY IN THE TREE AND ALREADY REFUTED:
	// voxel.March.ZCut engaged over 3.79e9 decisions at the horizon and skipped
	// 0.00%. The slab is about 37 chunks (~118 m) tall and a HORIZONTAL ray
	// never leaves it -- it was +2% WORSE at the horizon and -6% at sky. A Z or
	// column bound cannot reach horizontal rays, which is where the frame is
	// spent. This one is a 3D bound and reaches them.
	//
	// WHY NOT A DEEPER PYRAMID, also settled: our own spike measured a SECOND
	// coarse level moving miss-cost 56.5 -> 34.6 steps and hit-cost 48.6 ->
	// 68.9. Levoy 1990 published the rule -- a level pays only where
	// P(empty) x distance skipped exceeds the cost of the test, and that
	// product collapses at both ends of the ray. Literature at 0.55%
	// occupancy: one occupancy map 9.0x, a distance field on top +4.5%, a
	// third structure +0%. ONE LEVEL. Do not add a second.
	//
	// A PERMUTATION AND NOT A UNIFORM, which is the OPPOSITE choice to
	// voxel.March.ZCut two entries up, and the difference is the whole reason
	// that one argued its case. The Z bound adds a handful of ALU per SEGMENT
	// and no memory traffic, so an off-arm carrying its code carries nothing.
	// This arm adds TWO BUFFER LOADS PER BLOCK. A runtime branch would leave
	// that traffic in the control's binary, and a control that pays for loads
	// it never reads flatters whichever arm is under test -- which is exactly
	// what VOXEL_MARCH_SKIP_LEVELS' own note records and what VoxelMarchSpike
	// paid three legs to learn. 0 must be BYTE-IDENTICAL to today, and it is
	// only byte-identical if the code is not there.
	TAutoConsoleVariable<int32> CVarVoxelMarchBlockSkip(
		TEXT("voxel.March.BlockSkip"), 0,
		TEXT("THE COARSE OCCUPANCY LEVEL ABOVE THE CHUNK INDEX. 0 = off, THE CONTROL, and the ")
		TEXT("default. 1 = before stepping a chunk, consult a bit that says whether ANY of the ")
		TEXT("4x4x4 block of chunk cells around it is resident, and when none is, jump to the ")
		TEXT("block's far side in one step instead of walking 4 to 12 chunk cells through it.\n")
		TEXT("WHAT IT IS FOR: the chunk index is 99.7%% empty -- 50,052 resident chunks in a ")
		TEXT("16,777,216-cell, 64 MiB grid -- and empty space costs one scattered 4-byte load ")
		TEXT("into that 64 MiB per 3.2 m at level 0. voxel.March.ZCut cannot help here: it ")
		TEXT("engaged over 3.79e9 decisions at the horizon and skipped 0.00%%, because its slab ")
		TEXT("is ~118 m tall and a horizontal ray never leaves it. This bound is 3D.\n")
		TEXT("A PERMUTATION, NOT A UNIFORM, and deliberately unlike voxel.March.ZCut: this arm ")
		TEXT("adds two buffer loads per block, and a runtime branch would leave that traffic in ")
		TEXT("the control's binary and flatter the arm under test. Both arms must be COMPILED ")
		TEXT("separately; one binary must never be asked to behave two ways.\n")
		TEXT("IT CANNOT MAKE A HOLE, and that had to be built rather than hoped for. Two things ")
		TEXT("carry it. (1) A block is skipped only when NO cell in it is resident, so every ")
		TEXT("cell the jump passes is one the walk would have found absent anyway. (2) The jump ")
		TEXT("to the block's far side is biased conservatively SHORT -- a full chunk short, ")
		TEXT("applied AFTER the float conversion -- and is taken only when it still beats the ")
		TEXT("ordinary one-chunk advance, so the ray lands INSIDE the block it is leaving and ")
		TEXT("the chunk loop finishes the crossing. Danskin & Hanrahan measured why the bias is ")
		TEXT("needed: a ray taking even a tiny step can step through three pyramid nodes, so a ")
		TEXT("max/OR bound is NOT automatically conservative once you JUMP to the far side of ")
		TEXT("it.\n")
		TEXT("FALLTHROUGH SEMANTICS ARE PRESERVED EXACTLY. A skipped block sets ")
		TEXT("bCrossedAbsentChunk from its own AnyAbsent bit -- which is what the walk would ")
		TEXT("have set on the first non-resident chunk inside it -- and a block holding ANY ")
		TEXT("resident chunk is never skipped, so resident-empty chunks are still walked one by ")
		TEXT("one and still do NOT open the ladder. That distinction is why there are two bits ")
		TEXT("and not one: a resident-empty chunk is real air, and letting a coarser level stand ")
		TEXT("in for it plugs cave mouths, doorways and overhangs with rock.\n")
		TEXT("PROVE IT ENGAGED BEFORE BELIEVING A TIMING: with voxel.March.HoleStats on, ")
		TEXT("voxel.March.Stats prints blkConsulted / blkSkipped / blkCellsAvoided. ")
		TEXT("blkConsulted == 0 while this reads 1 means the arm is ARMED AND INERT and no ")
		TEXT("timing taken on that leg means anything -- and check blockFallback in the same ")
		TEXT("line, which counts frames that marched against an all-ones coarse level, i.e. ")
		TEXT("against the control.\n")
		TEXT("THOSE COUNTERS PROVE ENGAGEMENT AND DO NOT ESTIMATE THE SAVING. Iterations and ")
		TEXT("wall time move in opposite directions often enough to have been published three ")
		TEXT("times over -- VoxelRT measured iterations -60%% against traversal time +10-15%%, ")
		TEXT("and Aila & Laine record that ~20%% slower code gives the same measured perf. ")
		TEXT("THE SAVING IS `VoxelMarch.March` FROM ProfileGPU AND NOTHING ELSE. A leg where ")
		TEXT("blkCellsAvoided is enormous and that number does not move is a REAL NULL RESULT, ")
		TEXT("not a broken counter.\n")
		TEXT("WHAT WOULD REFUTE IT: any black arc, missing ground or terrain-through-terrain ")
		TEXT("that appears with this compiled at 1 and is absent at 0 on the same spawn and the ")
		TEXT("same pose. That is a hole, it outranks every millisecond here, and the correct ")
		TEXT("response is to turn the arm off -- not to shrink the block or widen the bias."),
		ECVF_RenderThreadSafe);

	// ---- THE SKY LADDER GATE (voxel.March.SkyLadder) ----------------------
	TAutoConsoleVariable<int32> CVarVoxelMarchSkyLadder(
		// RETIRED 2026-08-26 -- MEASURED, AND THE WIN WAS THE DEFECT.
//
// This arm read -7.6% on the marcher and it was buying that number by
// DELETING TERRAIN. Owner-visible: 96.33% of pixels changed and a near
// mountain was replaced by the valley behind it. After the mark was made
// honest (dilated across every level the ladder can retry into), the owner
// confirmed the pair identical -- and the timing inverted:
//
//     pitch    off        on(fixed)    delta      fallthrough taken
//       0    4.458 ms    4.595 ms     +3.1%      98.79% -> 89.09%
//     +30    5.587 ms    5.921 ms     +6.0%      99.54% -> 92.11%
//
// The correct mark licenses about a tenth as many declines as the broken one
// (9.7pp of retry rate instead of 62pp), and checking it costs more than those
// declines save. THE SAVING WAS THE SKIPPED WORK THAT WAS ACTUALLY NEEDED: the
// coarse stand-in a near mountain is MADE OF when its own-level chunks were
// never admitted.
//
// FOR AN ARM THAT CHANGES WHAT IS DRAWN, THE IMAGE COMES FIRST AND THE TIMING
// SECOND. This one was swept for timing, had a default set from that sweep, and
// was rebuilt -- all before anyone looked at a frame. Every one of those numbers
// was real and every one of them was worthless.
//
// Kept at default 0 with its machinery intact because the SKY MARK ITSELF is
// now verified correct (0 wrong marks in 398 sampled chunks / 9,950 columns,
// sampled against Amplifier::surfaceMm rather than the bound it was derived
// from) and may serve a different consumer. What is retired is using it to
// gate the retry ladder.
TEXT("voxel.March.SkyLadder"), 0,
		TEXT("WHAT OPENS THE FINE->COARSE RETRY LADDER. 0 = off, THE CONTROL, and the default: ")
		TEXT("the gate is bCrossedAbsentChunk, byte-identical to the shipped marcher. 1 = the ")
		TEXT("gate becomes 'did this ray cross an absence that is NOT PROVEN OPEN SKY'.\n")
		TEXT("THE POLARITY, AND GETTING IT BACKWARDS PLUGS CAVE MOUTHS, DOORWAYS AND OVERHANGS ")
		TEXT("WITH COARSE ROCK: a SKY-MARKED absence licenses NOT retrying. EVERY OTHER absence ")
		TEXT("-- pending, evicted, never-admitted, a torus alias's tag, a stale record, an ")
		TEXT("unconsulted cache -- must STILL RETRY. The predicate is written as 'absent AND NOT ")
		TEXT("PROVEN SKY' and never as 'not proven non-sky', so every bit pattern this feature ")
		TEXT("did not write falls on the retry side.\n")
		TEXT("WHAT IT IS FOR. The live gate asks 'did this ray cross ANYTHING not resident', and ")
		TEXT("the streamed set is a thin shell around the surface -- about 4.4 chunks per column ")
		TEXT("at level 0. Ring segment 0 starts AT THE CAMERA and the camera is in air, so the ")
		TEXT("first absent chunk on essentially every ray is that air, the gate opens at every ")
		TEXT("rung, and A SKY RAY WALKS THE CHUNK INDEX AT UP TO SEVEN LEVELS INSTEAD OF ONE. ")
		TEXT("Measured population, from this file's own note quoting ")
		TEXT("Saved/capture-zcut-alt120.log: 25.27%% of rays on a SETTLED STATIONARY world.\n")
		TEXT("AND IT ANSWERS THE QUESTION FOR FREE. VOXEL_MARCH_FALLTHROUGH_SHELL was written to ")
		TEXT("fix exactly this and is NOT COMPILED -- no permutation dimension, no SetDefine ")
		TEXT("anywhere in Source/ -- and its answer costs up to SIX extra scattered index loads ")
		TEXT("per non-resident chunk. This arm reads the coverage rule's own proof out of bits ")
		TEXT("the residency test already loaded: one mask, one compare, no loads.\n")
		TEXT("A PERMUTATION AND NOT A UNIFORM, unlike voxel.March.BlockSkySkip: this one adds a ")
		TEXT("field to the chunk cache and a fold at five sites in the walk, i.e. registers and ")
		TEXT("ALU in the inner loop, and a control that carries the treatment's work flatters ")
		TEXT("the treatment. Rings + fallthrough > 0 only; other combinations are refused at ")
		TEXT("compile rather than built and never selected.\n")
		TEXT("bCrossedAbsentChunk IS UNTOUCHED at all five of its write sites, so `uncovered`, ")
		TEXT("the hole metric and the shell counters read on this arm exactly what they read on ")
		TEXT("the control. This arm adds a SECOND flag and changes only which one the gate reads.\n")
		TEXT("PROOF OF TRAFFIC, AND NOTHING ELSE ON THE LEG MEANS ANYTHING WITHOUT IT: ")
		TEXT("FallthroughTaken / FallthroughConsidered, printed by voxel.March.Stats with ")
		TEXT("voxel.March.HoleStats on, MUST FALL between the control and this arm. If it does ")
		TEXT("not move, THE SWITCH IS INERT -- report that, not a millisecond. Seven switches in ")
		TEXT("this repo were inert while looking armed. It also requires the STREAMING side: ")
		TEXT("with voxel.Stream.SkyMark 0 no cell is marked, every absence reads non-sky, and ")
		TEXT("this arm is byte-identical to the control BY CONSTRUCTION -- which is a correct ")
		TEXT("null, not a bug, and the skyMark lines on the LogVoxelPerf log say which it is.\n")
		TEXT("WHAT WOULD REFUTE IT: `uncShell` RISING against the control. That would mean the ")
		TEXT("sky mark is being read over REAL gaps and this gate is dropping fills the owner ")
		TEXT("can see. `uncovered` collapsing while `uncShell` holds is the intended reading. ")
		TEXT("And a screenshot outranks both: any black arc, missing ground, or coarse rock in a ")
		TEXT("cave mouth that appears at 1 and is absent at 0 on the same spawn and pose is a ")
		TEXT("hole, and the response is to turn the arm off.\n")
		TEXT("IT HAS ALREADY BEEN REFUTED ONCE, and both causes are fixed rather than tuned. ")
		TEXT("2026-08-25, pinned pose, deterministic harness (control-vs-control 0.048%%): armed ")
		TEXT("with voxel.Stream.SkyMarkChunks 32 the frame differed from the control by 96.33%% of ")
		TEXT("pixels -- a near mountain gone and the valley behind it drawn. (1) A level-L mark ")
		TEXT("was declining a retry at level L+1, which it cannot speak for; the writer now takes ")
		TEXT("its bound as a MAX over a dilated set covering every level this switch can retry ")
		TEXT("into, and it READS voxel.March.Fallthrough to know how far that is. (2) This gate ")
		TEXT("read a TRUNCATED walk as evidence -- the hierarchical walk caps its chunk loop at ")
		TEXT("512 and a near-vertical ray needs 1,310 -- so a truncated walk now always reopens ")
		TEXT("the ladder.\n")
		TEXT("FOR AN ARM THAT CHANGES WHAT IS DRAWN, THE IMAGE COMES FIRST AND THE TIMING SECOND. ")
		TEXT("The band was swept for timing, the knee became the default, the binary was rebuilt ")
		TEXT("-- all before the visual check. The timing was real and correct; the arm was ")
		TEXT("deleting terrain at every band it was measured at. Shoot the control/armed pinned ")
		TEXT("pose pair FIRST and require it to match to within the harness's own noise floor; ")
		TEXT("only then does a millisecond off this switch mean anything."),
		ECVF_RenderThreadSafe);

	// ---- THE SKY LICENCE (voxel.March.BlockSkySkip) -----------------------
	TAutoConsoleVariable<int32> CVarVoxelMarchBlockSkySkip(
		TEXT("voxel.March.BlockSkySkip"), 0,
		TEXT("WHAT LICENSES A BLOCK SKIP. Read only inside a kernel compiled with ")
		TEXT("voxel.March.BlockSkip 1; with that off this changes nothing and the marcher is ")
		TEXT("the byte-identical control.\n")
		TEXT("  0 = RESIDENCY ONLY, the default and byte-identical to the shipped BlockSkip arm: ")
		TEXT("a block is skipped when no cell in it is resident. No sky buffer is loaded at all ")
		TEXT("(the load sits under a uniform branch), so this arm keeps EXACTLY the memory ")
		TEXT("traffic its published result was measured with.\n")
		TEXT("  1 = BOTH: unoccupied OR provably sky.\n")
		TEXT("  2 = SKY ONLY: residency alone stops licensing a skip.\n")
		TEXT("READ THIS BEFORE TAKING A TIMING OFF 1 OR 2 -- THIS ARM CANNOT WIN, BY PROOF, AND ")
		TEXT("IS AN INSTRUMENT RATHER THAN A TREATMENT.\n")
		TEXT("The all-sky bit is set only when all 64 cells are marked AND NONE IS RESIDENT ")
		TEXT("(FVoxelMarchChunkIndex::RefreshBlockBits), so AllSky IMPLIES !Occupied. Therefore ")
		TEXT("mode 1's predicate (!Occupied || AllSky) is IDENTICAL to mode 0's (!Occupied) -- ")
		TEXT("not similar, identical -- and mode 2's is a strict SUBSET of it. Every block this ")
		TEXT("arm skips is one the shipped residency arm already skipped, so it can only skip ")
		TEXT("FEWER blocks and can never be faster. Any timing difference between 0 and 1 on ")
		TEXT("this switch is run-to-run noise and must be reported as such.\n")
		TEXT("WHY IT EXISTS ANYWAY: the block level is the wrong consumer for the mark, and this ")
		TEXT("is how that was established rather than assumed. What these modes DO buy is the ")
		TEXT("measurement -- blkSkyLicensed of blkConsulted says what FRACTION of the space a ")
		TEXT("ray crosses is PROVABLY empty rather than merely unstreamed, and the run mean says ")
		TEXT("whether that space clusters. Those two numbers are what a future coarser skip would ")
		TEXT("be sized from. The mark's actual payoff is on the RETRY LADDER (see ")
		TEXT("voxel.March.SkyLadder), where 'this absence is air, not a gap in held ground' ")
		TEXT("changes which rays walk the index seven times instead of once.\n")
		TEXT("WHAT THE RESIDENCY-ONLY ARM ACTUALLY MEASURED, corrected 2026-08-25: it skipped ")
		TEXT("24.74%% of 11.1e9 blocks and ran 30%% SLOWER, paying 23.7 block tests per ray to ")
		TEXT("avoid 11.2 cells. The failure was NOT that the bit could not license an advance -- ")
		TEXT("read the traversal, it jumps on !Occupied and always has. The failure was the ")
		TEXT("ARITHMETIC: the tests cost more than the cells they removed. A narrower predicate ")
		TEXT("does not change that arithmetic in the helpful direction, which is why 1 and 2 ")
		TEXT("below are instruments and not treatments.\n")
		TEXT("A UNIFORM AND NOT A PERMUTATION, which departs from voxel.March.BlockSkip's own ")
		TEXT("doctrine and is argued rather than assumed: that doctrine exists so a control does ")
		TEXT("not carry the treatment's LOADS, and mode 0 issues no sky load (uniform branch, ")
		TEXT("coherent across the dispatch). What it buys is all three states on ONE binary, ")
		TEXT("interleavable in one leg -- which is exactly how the -30%% figure this has to beat ")
		TEXT("was NOT measured.\n")
		TEXT("IT CANNOT MAKE A HOLE, and that is built rather than hoped for. The sky bit is set ")
		TEXT("only when ALL 64 cells carry a valid open-sky mark AND none is resident; the marks ")
		TEXT("come from IsChunkProvablyAllAir, a worldgen-grade proof already verified in ")
		TEXT("production by -VoxelVerifySkyBand; a block's 64 cells share one torus tag ")
		TEXT("(128 %% 4 == 0), so a mark from a coord 128 cells away resets the block's count ")
		TEXT("rather than licensing it; and every streaming transition on a cell overwrites the ")
		TEXT("mark and withdraws the licence BEFORE the cell stops saying so. Unbound, the ")
		TEXT("buffer reads zeros = nothing is sky = the arm is inert, the opposite of the ")
		TEXT("occupancy pair's failure direction.\n")
		TEXT("FALLTHROUGH SEMANTICS ARE PRESERVED EXACTLY, unchanged from mode 0: a skipped ")
		TEXT("block still sets bCrossedAbsentChunk from its own AnyAbsent bit, so the ")
		TEXT("fine->coarse ladder retries exactly the rays it does today. A sky-marked cell is ")
		TEXT("STILL NON-RESIDENT and must still open that ladder -- the mark says the streaming ")
		TEXT("system chose not to hold it, not that the ladder should stop caring.\n")
		TEXT("PROVE IT ENGAGED BEFORE BELIEVING A TIMING. With voxel.March.HoleStats on, ")
		TEXT("voxel.March.Stats prints blkSkyLicensed of blkConsulted and the run mean. ")
		TEXT("blkSkyLicensed == 0 with blkConsulted > 0 means the marcher found NO licensed ")
		TEXT("block, which is the streaming writer not running -- check voxel.Stream.SkyMark is ")
		TEXT("1 and read the skyMark CELLS line, where written=0 of offered>0 names the cause. ")
		TEXT("A run mean near 1.0 against a high licensed rate says sky is SCATTERED at 4^3 ")
		TEXT("granularity, which indicts the block size, not the mark.\n")
		TEXT("THOSE COUNTERS ARE DECISIONS, NOT NANOSECONDS. THE SAVING IS `VoxelMarch.March` ")
		TEXT("FROM ProfileGPU AND NOTHING ELSE. A leg where the licensed rate is large and that ")
		TEXT("number does not move is a REAL NULL RESULT -- and it would be the FOURTH here, so ")
		TEXT("report it as one rather than looking for a counter to blame.\n")
		TEXT("WHAT WOULD REFUTE IT: any black arc, missing ground or sky where terrain belongs ")
		TEXT("that appears at 1 or 2 and is absent at 0 on the same spawn and the same pose. ")
		TEXT("That is a hole, it outranks every millisecond here, and the response is to turn ")
		TEXT("the arm off and read skyVerifyBad -- not to shrink the band."),
		ECVF_RenderThreadSafe);

	// ======================================================================
	// THE TERRAIN HEIGHT PYRAMID
	// ======================================================================
	//
	// A max-reduced 2D upper bound on terrain height (VoxelHeightPyramid.h),
	// consulted by a hierarchical DDA before the voxel walk. ONE traversal
	// answers all three directions: a rising ray terminates once it is above
	// every bound left along it, a level ray steps over a valley in coarse
	// jumps, and a descending ray jumps straight to first contact.
	//
	// WHY THIS AND NOT voxel.March.ZCut, WHICH ALREADY BOUNDS Z. The Z slab is
	// ONE PAIR OF NUMBERS PER LEVEL, so a horizontal ray sits inside it for its
	// whole length BY CONSTRUCTION. Measured: the slab skips 0.00% of decisions
	// at the flight's pitch -10 across 3.3e9 consultations, 21% at a sky pose,
	// 93% at 1,200 m altitude. The horizon -- which is where the frame is spent,
	// 4.45 ms of it -- is precisely the case it cannot touch. A heightfield is a
	// FUNCTION OF POSITION, so it bounds every direction: a level ray over a
	// valley 700 m below is provably in air for that whole stretch.
	//
	// AND WHY NOT A SINGLE CEILING, WHICH WOULD BE CHEAPER. A ceiling must take
	// the MAX over the whole reach. At the leg spawn that pins it at ~2,860 m
	// from one distant massif while local ground drops to 1,489 m -- it throws
	// away 1,371 m of bound in every direction that does not point at the
	// massif, and the camera sits 667 m BELOW it. It is not a cheaper first
	// step here; it is a different and much weaker mechanism.
	TAutoConsoleVariable<int32> CVarVoxelMarchHeightPyramid(
		TEXT("voxel.March.HeightPyramid"), 0,
		TEXT("THE TERRAIN HEIGHT PYRAMID. 0 = off, THE CONTROL, and the default. 1 = before ")
		TEXT("walking, run a hierarchical DDA over a max-reduced upper bound on terrain height ")
		TEXT("and hand the voxel walk only the intervals where a hit is POSSIBLE.\n")
		TEXT("A UNIFORM AND NOT A PERMUTATION, for the reason voxel.March.ZCut is one: the arm ")
		TEXT("adds no load an off-arm would have to carry, and one build then serves both arms ")
		TEXT("of a pitch sweep, which is how this is measured.\n")
		TEXT("IT CANNOT MAKE A HOLE, and that property is built rather than hoped for. Every ")
		TEXT("cell holds a PROVABLE upper bound from the amplifier's own contract (caves, ")
		TEXT("caverns and karst only ever CARVE, so they cannot break an upper bound; asset ")
		TEXT("crowns are composed in per layer with reach dilation). Anything absent, declined, ")
		TEXT("not yet filled or outside the field holds +INFINITY, which makes the air test ")
		TEXT("false and the walk proceed exactly as the control does. THE ONE THING THE BOUND ")
		TEXT("CANNOT SEE IS A PLAYER EDIT, and the builder folds EditedFootprintMaxZ back in ")
		TEXT("for that.\n")
		TEXT("WHAT WOULD REFUTE IT: any black arc, missing ground, or sky where terrain belongs ")
		TEXT("that appears at 1 and is absent at 0 on the same spawn and the same pose. That is ")
		TEXT("a hole, it outranks every millisecond here, and the response is to turn the arm ")
		TEXT("off -- not to add a margin.\n")
		TEXT("READ THE ENGAGEMENT COUNTERS BEFORE ANY TIMING. voxel.March.Stats prints ")
		TEXT("heightConsulted / heightAdvanced / heightEmpty / heightReentries. heightConsulted ")
		TEXT("== 0 while this reads 1 is the arm ARMED AND INERT. heightReentries == 0 with ")
		TEXT("heightAdvanced large means only the tStart half is running and the mid-ray skips ")
		TEXT("were NOT measured, which is a PARTIAL result and must be reported as one."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchHeightPyramidVerify(
		TEXT("voxel.March.HeightPyramid.Verify"), 0,
		TEXT("THE FALSIFIER, and it is the gate that runs FIRST. 0 = off, the default. 1 = every ")
		TEXT("ray ALSO walks its full unclamped interval, and any hit found inside space the ")
		TEXT("pyramid declared empty is counted. Two counters, not one, because they mean ")
		TEXT("different things: heightMissed is the clamped walk finding NOTHING where the ")
		TEXT("control found geometry -- an unambiguous hole -- while heightLate is a hit ")
		TEXT("further along than the control, which may only be a coarser ring answering a ")
		TEXT("segment after a mid-ray restart. heightMissed MUST be 0; heightLate is judged ")
		TEXT("on its DISTRIBUTION, which is printed beside it.\n")
		TEXT("A CONFIRMATION THAT CANNOT COME OUT THE OTHER WAY IS NOT ONE. Before believing a ")
		TEXT("zero here, set voxel.March.HeightPyramid.BiasM to 50 -- that lowers every bound by ")
		TEXT("50 m, i.e. deliberately claims air where there is ground -- and confirm ")
		TEXT("BOTH counters go NON-ZERO. A run that has not done that has not tested ")
		TEXT("anything.\n")
		TEXT("COSTS ROUGHLY 2x: the ray is walked twice by construction. Never read timing from ")
		TEXT("a leg with this on."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarVoxelMarchHeightPyramidBiasM(
		TEXT("voxel.March.HeightPyramid.BiasM"), 0.0f,
		TEXT("METRES SUBTRACTED FROM EVERY HEIGHT BOUND THE SHADER READS. 0 = the sound field ")
		TEXT("and the only value that may ever ship.\n")
		TEXT("THIS EXISTS TO BREAK THE FEATURE ON PURPOSE. A positive value makes the bound too ")
		TEXT("LOW, which is exactly the failure this design is written to prevent -- proven air ")
		TEXT("over real ground -- so it is the corruption that proves the falsifier above can ")
		TEXT("fire. Set it with Verify 1 and watch heightMissed climb; set it with Verify 0 ")
		TEXT("and watch terrain disappear from the image. Both are the point.\n")
		TEXT("A NEGATIVE VALUE IS A MARGIN, AND MARGINS ARE REFUSED HERE. Asset crowns are ")
		TEXT("already composed into the bound per layer with reach dilation; a global pad on top ")
		TEXT("of that is strictly looser than the per-footprint composition the codebase already ")
		TEXT("computes. Negative values are clamped to 0."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchHeightPyramidMaxIters(
		TEXT("voxel.March.HeightPyramid.MaxIters"), 3,
		TEXT("How many separate voxel-walk intervals one ray may be split into by the pyramid. ")
		TEXT("THIS IS THE MID-RAY SKIP KNOB and it is the difference between a tStart advance ")
		TEXT("and a real maximum-mipmap traversal.\n")
		TEXT("1 = tStart only: skip forward to the first cell where a hit is possible, then hand ")
		TEXT("the walk everything from there to the far bound. A ray that grazes a ridge then ")
		TEXT("pays for all the air beyond it.\n")
		TEXT("Greater than 1 = the walk is re-entered after a miss, having skipped the next ")
		TEXT("proven-empty stretch. THIS IS THE HORIZON CASE and it is what the 4.45 ms is ")
		TEXT("spent on.\n")
		TEXT("EACH RE-ENTRY COSTS A FULL TRAVERSAL SETUP, so this is not free and is not ")
		TEXT("monotonic -- measure it, do not raise it on principle. On the LAST permitted ")
		TEXT("iteration the walk is handed the whole remaining interval, so lowering this can ")
		TEXT("only cost speed and can never make a hole."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchZCutPadChunks(
		TEXT("voxel.March.ZCutPadChunks"), 4,
		TEXT("How many EXTRA chunks, at that level's own chunk size, the resident-Z slab is ")
		TEXT("widened by on each side before it is used to cut. Clamped to a minimum of 1 and ")
		TEXT("that minimum is LOAD-BEARING, not defensive tidiness: at pad 0 a removed chunk ")
		TEXT("could sit face-adjacent in Z to a resident one, VoxelMarchAbsentTouchesShell would ")
		TEXT("have answered TRUE for it, and skipping it would silently drop a ")
		TEXT("bCrossedShellAbsent the retry ladder gates on -- changing which rays retry, which ")
		TEXT("is a hole risk rather than a speed one. At pad >= 1 every removed chunk is at least ")
		TEXT("two chunks away in Z from anything resident and all six of that test's neighbour ")
		TEXT("probes are provably non-resident.\n")
		TEXT("4 RATHER THAN 1 buys freshness as well: the bound is read on the render thread from ")
		TEXT("state the game thread widens, so a chunk admitted just outside the previous frame's ")
		TEXT("union is covered by the pad rather than by an ordering argument. 4 chunks is 12.8 m ")
		TEXT("at level 0 and 819 m at level 6, against a level-0 shell about 14 m thick -- so the ")
		TEXT("pad is the dominant term at the near levels and rounding noise at the far ones, ")
		TEXT("which is the correct way round.\n")
		TEXT("IT ACTS IN INTEGER CHUNK SPACE, BEFORE THE SHADER CONVERTS THE SLAB TO WORLD UU, ")
		TEXT("and it is therefore NOT the guard against float error in that conversion or in ")
		TEXT("the divide by Dir.z that follows it. THAT guard is a SEPARATE outward bias of one ")
		TEXT("full chunk applied to the t-interval AFTER the conversion -- see THE OUTWARD BIAS ")
		TEXT("in VoxelBrickTraverse.ush. The two are deliberately not the same number: this one ")
		TEXT("is load-bearing for the bCrossedShellAbsent argument and that one for arithmetic, ")
		TEXT("and a single knob doing both jobs would break both at once when it was lowered.\n")
		TEXT("RAISING IT IS ALWAYS SAFE AND ALWAYS COSTS SPEED. LOWERING IT BELOW 1 IS REFUSED."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchRingCount(
		TEXT("voxel.March.RingCount"), 0,
		TEXT("How many rings the cascade walks. 0 (default) = FOLLOW THE STREAMING CASCADE ")
		TEXT("(GetMaxRingLevel()+1, via FVoxelMarchChunkIndex::GetStreamedRingLevels) -- 6 on a ")
		TEXT("default run, 7 under -VoxelMaxRingLevel=6, so the 8 km ring cannot stream without ")
		TEXT("being walked. Explicit 1..7 overrides for measurement; 2 is the B-2b-1 ")
		TEXT("configuration that proved the mechanism (0-128 / 128-256 m). Clamped to the ")
		TEXT("levels the chunk index actually carries -- asking for more would walk a level with ")
		TEXT("no grid behind it, which reads as empty space rather than as an error.\n")
		TEXT("Only meaningful with voxel.March.Rings 1."),
		ECVF_RenderThreadSafe);

	// THE CLIMATE TINT'S STRENGTH, on a cvar because it is the term most likely
	// to want tuning by eye. 0 makes the marcher byte-identical to before it.
	TAutoConsoleVariable<float> CVarVoxelMarchClimateStrength(
		// DEFAULT 0 SINCE 2026-08-22, at the owner's request. The terrain is
		// rendering with blue speckling that gets stronger at coarser LODs and
		// is worst directly under the camera, and this tint is the newest thing
		// in the shading path -- so it is off by default until the speckling
		// has been judged with it out of the picture. Set to 1 to A/B it: if
		// the blue returns, this is the cause; if it does not, the cause is the
		// material lookup or the GI probe and this was never implicated.
		TEXT("voxel.March.ClimateStrength"), 0.0f,
		TEXT("Strength of the marcher's per-chunk climate tint (temperature/precipitation from "
		     "the chunk record's dword 7). 0 = off and byte-identical to no tint. 1 = full. "
		     "The quad path feeds these two bytes to M_VoxelTerrain's biome graph instead; the "
		     "marcher shades per-voxel (ADR-0008), so this is a deliberate re-design and a dial, "
		     "not a transcription."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarVoxelMarchRingOuterM(
		// 64 AS OF 2026-08-25 -- THIS MUST TRACK kDefaultRingPresets AND THERE IS
		// NO ASSERT THAT IT DOES. The marcher derives every ring boundary from
		// this ONE uniform as OuterUU(L) = R0 * 2^L; residency derives its own
		// from the presets. If they disagree the marcher asks for levels at
		// radii the pool never populates, and that is a HOLE, not an error.
		TEXT("voxel.March.RingOuterM"), 64.0f,
		TEXT("Outer radius of ring 0 in METRES; ring 1 runs from there to twice it. 128 gives ")
		TEXT("the 0-128 / 128-256 split of kDefaultRingPresets, which is what residency actually ")
		TEXT("builds, and moving it away from the preset makes the marcher ask for levels at radii ")
		TEXT("the pool does not populate -- a hole, not an error."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarVoxelMarchReachM(
		TEXT("voxel.March.ReachM"), 0.0f,
		TEXT("How far the marcher may see, in METRES. 0 = use the occupancy volume's own 51.2 m ")
		TEXT("box, which is the default and the only setting under which the source A/B is ")
		TEXT("valid.\n")
		TEXT("WHY IT EXISTS: a skip ratio measured over 25.6 m badly understates the 4 km case, ")
		TEXT("because skipping wins more the longer the ray. R0 spans 0-128 m, so level 0 alone ")
		TEXT("supports a 5x longer ray with NO ring-transition logic -- which is what lets the ")
		TEXT("project's load-bearing number arrive before its largest piece of engineering.\n")
		TEXT("SOURCE 1 ONLY above 25.6 m. The occupancy volume is 51.2 m across and cannot ")
		TEXT("follow, so the control arm is gone at longer reach and a regression could no longer ")
		TEXT("be attributed to the source rather than the walk. Requesting it on source 0 is ")
		TEXT("refused and says so.\n")
		TEXT("AND IT BREAKS DEPTH-GATE COMPARABILITY with every run recorded so far: `miss` ")
		TEXT("collapses and the judged population changes character. Do not compare gate numbers ")
		TEXT("across this line."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchStepBudget(
		TEXT("voxel.March.StepBudget"), 3328,
		TEXT("DDA step cap per ray, clamped to [1, 4096]. 886 = 512*sqrt(3), the full diagonal of ")
		TEXT("the occupancy volume, which is the arm the Phase 0 gate was measured at. A budget ")
		TEXT("too small for the pose does not error -- rays terminate early and the terrain simply ")
		TEXT("stops, which is why the census counts budget-exhausted rays as their own outcome ")
		TEXT("rather than folding them into misses."),
		ECVF_RenderThreadSafe);

	// ---- the three bisection switches -------------------------------------
	//
	// Each is a shader PERMUTATION and not a uniform, and that is not tidiness.
	// A uniform branch leaves the other side's texture loads in the binary, so
	// turning a feature "off" to bisect a defect would still pay its memory
	// traffic and still perturb the measurement being bisected. The same
	// argument VoxelMarchSpike.usf makes for its no-fetch arm.

	TAutoConsoleVariable<int32> CVarVoxelMarchAO(
		TEXT("voxel.March.AO"), 1,
		TEXT("Per-voxel corner ambient occlusion, multiplied into base colour. 1 = on, 0 = off ")
		TEXT("(the control for 'is the terrain too dark'). ")
		TEXT("INTO BASE COLOUR, NOT INTO GBufferAO, and that is not a shortcut. GBufferC.a is only ")
		TEXT("GBufferAO when ALLOW_STATIC_LIGHTING is 0; with static lighting allowed -- the engine ")
		TEXT("default and this project's setting -- the channel carries encoded indirect irradiance ")
		TEXT("and the decoder returns GBufferAO = 1 unconditionally, so raw AO written there ")
		TEXT("darkens nothing and corrupts the irradiance term. Base colour is also where the ")
		TEXT("SHIPPED RASTER PATH puts it (VoxelQuadVertexFactory.ush:879), which is what makes a ")
		TEXT("marcher-vs-quads colour A/B mean anything. ")
		TEXT("The corner rule is byte-identical to vxc::detail::aoCorner (mesher.h:33) over the ")
		TEXT("same eight neighbours; the marcher evaluates it for the exact voxel the ray hit ")
		TEXT("instead of spreading four corner values over a 64-voxel greedy quad."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchDBuffer(
		TEXT("voxel.March.DBuffer"), 0,
		TEXT("Apply DBuffer decals by hand in the emit. DEFAULT 0 AND IT CANNOT CURRENTLY BE 1 -- ")
		TEXT("see the long note in VoxelMarch.usf. The DBuffer textures live in ")
		TEXT("Renderer/Private/DBufferTextures.h and there is no RENDERER_API path to them from a ")
		TEXT("game module at this hook; FSceneTextureUniformParameters does not carry them and ")
		TEXT("ESceneTextureSetupMode has no flag for them. The shader code is written and correct ")
		TEXT("and waits on one binding. Setting this to 1 logs once and stays off, rather than ")
		TEXT("silently sampling black and calling it 'no decals'."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchVelocity(
		TEXT("voxel.March.Velocity"), 1,
		TEXT("Write motion vectors for marched terrain. 1 = on. OFF IS NOT A FREE CONTROL: TSR ")
		TEXT("reprojects with whatever is in the velocity buffer, and terrain that writes nothing ")
		TEXT("gets the camera-only reprojection, which is correct for static ground and wrong the ")
		TEXT("moment the streaming origin rebases. Use 0 only to bisect a smearing artefact."),
		ECVF_RenderThreadSafe);

	// ---- HALF-RESOLUTION MARCHING -----------------------------------------
	//
	// A PERMUTATION, like the three bisection switches above, and for the same
	// reason plus one more: at half res the VisBuffer is PHYSICALLY half-size,
	// so a uniform branch would leave a shader able to index it at full-res
	// coordinates. The size of the buffer and the shader that reads it are
	// decided together or not at all.
	TAutoConsoleVariable<int32> CVarVoxelMarchHalfRes(
		TEXT("voxel.March.HalfRes"), 0,
		TEXT("*** REJECTED BY THE OWNER 2026-08-27. FINAL. DEFAULT 0 AND IT STAYS 0. ***\n")
		TEXT("Do not re-propose this, and do not re-propose any other RAY-COUNT reduction as a ")
		TEXT("route to the frame-rate target -- read the four reasons before spending a day on ")
		TEXT("one:\n")
		TEXT("  1. THE IMAGE. Judged four times, at two resolutions, on stills and in motion, and ")
		TEXT("     rejected every time. The last judgement was made AFTER the depth defect below ")
		TEXT("     was fixed, so it is a verdict on the FEATURE and not on a bug: 'still looks bad ")
		TEXT("     for more subtle blurring and pixelation effects'. That is inherent -- one ray ")
		TEXT("     per 2x2 means normal, material and AO come from a quarter as many samples, and ")
		TEXT("     only DEPTH was ever exact. No temporal trick recovers it; the lattice jitter ")
		TEXT("     was built, measured and does not.\n")
		TEXT("  2. IT DOES NOT REACH THE TARGET ANYWAY. gate= reads GOAL3-FAIL on all four arms, ")
		TEXT("     half-res included. It moves p95 from 66 to 82 fps against a 100 fps bar.\n")
		TEXT("  3. IT IS NOT ON THE CRITICAL PATH. The remaining gap is the MOVING TAIL, and the ")
		TEXT("     half-res A/B is itself the proof that the marcher is not in it: halving ray ")
		TEXT("     count moved the moving p99 delta by 0%%. The tail is game-thread chunk ")
		TEXT("     publication -- gameMs 2.84 -> 12.35 ms FAST -> p99 against 8.25 ms of slack.\n")
		TEXT("  4. THE SPEED DIFFERENCE BETWEEN THE THREE HALF-RES VARIANTS IS NOISE. Static, ")
		TEXT("     jitter-4 and Halton-8 land within 0.4 ms of each other and the ordering FLIPPED ")
		TEXT("     between two runs of the same binary. There is nothing to tune here.\n")
		TEXT("WHAT SURVIVES AND IS WORTH KEEPING: the code stays, off, because the DEPTH FIX in ")
		TEXT("VoxelMarch.usf is a real correctness fix and the measurements are worth more than ")
		TEXT("the lines. If anyone ever revisits this, the holes are already diagnosed -- do not ")
		TEXT("re-derive them.\n")
		TEXT("---- what it does ----\n")
		TEXT("One march ray per 2x2 block instead of one per pixel, with the FULL-RES DEPTH ")
		TEXT("RECONSTRUCTED EXACTLY in the emit rather than upsampled. 0 = off and byte-identical ")
		TEXT("to every leg before this arm existed; 1 = on.\n")
		TEXT("WHY IT IS WORTH A SWITCH. The march is the render floor -- 5.42 ms of a 9.55 ms ")
		TEXT("parked frame -- and its cost is RAY-COUNT LINEAR: the 2026-08-25 screen-percentage ")
		TEXT("sweep measured 4.37 / 3.99 / 3.91 ms per Mray across a 5.7x range, linear within ")
		TEXT("2%%. Every tuning knob is spent (StepBudget flat across a 4x cut, SkipLevels 1 and 2 ")
		TEXT("byte-identical, AO free, Velocity free, ReachM inert), and the only knob that moved ")
		TEXT("cost -- RingCount -- pays for it in draw distance, which is refused. Quartering the ")
		TEXT("rays is the structural version of the same lever and it costs NO view distance.\n")
		TEXT("WHY THE DEPTH IS NOT MERELY UPSAMPLED. Every quantity the emit needs is a property ")
		TEXT("of the HIT VOXEL AND FACE, and the hit surface is an axis-aligned PLANE -- so for a ")
		TEXT("pixel whose true hit lies on the same face as a neighbouring sample's, the exact t ")
		TEXT("is one ray/plane divide. AO stays pixel-exact and velocity stays exact because both ")
		TEXT("descend from that t. Error is confined to pixels whose true voxel is none of the ")
		TEXT("four candidates -- silhouettes and sub-sample features -- which fall back to plain ")
		TEXT("nearest-neighbour.\n")
		TEXT("HOW TO PROVE IT ENGAGED RATHER THAN ASSUME IT. Three readings, and take all three: ")
		TEXT("(1) the '[voxel-march] halfres:' line below, printed unconditionally whenever the ")
		TEXT("shape changes, which names the dispatch and the VisBuffer extent the march actually ")
		TEXT("ran; (2) voxel.March.Stats 'tiles total/drawn', which fall ~4x because the emit tile ")
		TEXT("is 16x16 full-res pixels instead of 8x8 -- and 'drawn' is a GPU READBACK, not a ")
		TEXT("CPU count; (3) voxel.March.VerifyDepth 1 in mode 2, which under this arm grades the ")
		TEXT("RECONSTRUCTION at full res against the raster depth.\n")
		TEXT("THE FALSIFIER, PRE-REGISTERED: the gate's INTERIOR (non-edge) disagreement rate must ")
		TEXT("be ~0. A silhouette may disagree -- both renderers are point samples of a step ")
		TEXT("function there -- but an interior pixel has no excuse available, so a non-zero ")
		TEXT("interior rate means the reconstruction is WRONG and refutes this arm rather than ")
		TEXT("qualifying it. Read 'edge' beside 'compared' so the exemption's size stays visible.\n")
		TEXT("WHAT IS DELIBERATELY NOT DONE: the cone slope is NOT doubled, so the march samples ")
		TEXT("the same LOD at half the density. That keeps the first measurement about the ")
		TEXT("reconstruction alone. If the gate shows an interior rate that grows with distance, ")
		TEXT("the doubled slope is the change to try -- see VoxelMarch.usf. And there is no ")
		TEXT("silhouette fixup pass yet, on purpose: its size should come from a measured fallback ")
		TEXT("rate, not a guess."),
		ECVF_RenderThreadSafe);

	// ---- THE TEMPORALLY VARYING SAMPLE LATTICE ----------------------------
	//
	// A SECOND SWITCH RATHER THAN A CHANGE TO THE FIRST, so a leg measures THREE
	// arms and not two: full-res, half-res-static, half-res-jittered. Folding it
	// into voxel.March.HalfRes would delete the static arm, which is the control
	// the owner's rejection was recorded against and therefore the one thing
	// that must survive.
	//
	// NOT A PERMUTATION. Unlike HalfRes itself, this changes no buffer extent,
	// no dispatch shape and no tile size -- only a float2 uniform that every
	// permutation already binds. The march, the depth pre-emit, the GBuffer emit
	// and the verify gate all read the SAME uniform from the SAME frame's
	// resolve, so there is no second construction of the lattice to drift.
	TAutoConsoleVariable<int32> CVarVoxelMarchHalfResJitter(
		TEXT("voxel.March.HalfRes.Jitter"), 0,
		TEXT("MOVE THE HALF-RES SAMPLE LATTICE INSIDE ITS 2x2 BLOCK, ONE POSITION PER FRAME, so ")
		TEXT("TSR has something new to accumulate. 0 = the static lattice this arm shipped with ")
		TEXT("(sample at the block centre, byte-identical to every half-res leg so far); 1 = a ")
		TEXT("FOUR-FRAME ROTATION over the four full-res pixel centres; 2 = an EIGHT-FRAME ")
		TEXT("HALTON(2,3) over the same +/-0.5 px box. Ignored at voxel.March.HalfRes 0, where ")
		TEXT("the uniform is forced to zero -- a jittered FULL-res march would hand the emit a t ")
		TEXT("measured along a different ray than the one the emit rebuilds, which is a wrong ")
		TEXT("depth on every pixel rather than a softer one.\n")
		TEXT("WHAT IT IS FOR. The owner rejected half res on quality: 'the half res distant ")
		TEXT("terrain does not look good. Very grainy and low res', 25.85%% of pixels differing ")
		TEXT("at a pinned pose. In the far field about one voxel projects to one FULL-RES pixel, ")
		TEXT("so one half-res sample stands for about four voxels and the reconstruction's ")
		TEXT("nearest-neighbour fallback -- an edge case up close -- becomes the common case out ")
		TEXT("there. Depth was always the only quantity this arm called exact; which voxel ")
		TEXT("supplies the normal, material and AO is resolved from four samples over four times ")
		TEXT("the area, so the SHADING at distance is quarter res. A lattice that moves converts ")
		TEXT("a blocky point sample into a temporally dithered one, which is what checkerboard ")
		TEXT("rendering and temporal upsampling do in shipped titles.\n")
		TEXT("HOW TO PROVE IT ENGAGED RATHER THAN ASSUME IT, and take both: (1) the ")
		TEXT("'[voxel-march] jitter:' line, printed once per DISTINCT PHASE actually dispatched, ")
		TEXT("so a four-frame cycle produces four lines with four different offsets and a static ")
		TEXT("lattice produces exactly one; (2) voxel.March.Stats 'halfResJitter', which prints ")
		TEXT("phasesSeen and the phase mask -- phasesSeen=1 on a settled leg means the lattice is ")
		TEXT("STATIC and every image taken since describes the arm this was meant to replace.\n")
		TEXT("THE FALSIFIER, PRE-REGISTERED. INERT: the offset does not change between frames. ")
		TEXT("REFUTED: it does change and the far-field graininess is unchanged on a pinned-pose ")
		TEXT("A/B -- then temporal accumulation is not the cure for a four-voxels-per-sample ")
		TEXT("undersample and this should be REVERTED rather than tuned. WORSE: any new shimmer ")
		TEXT("or crawl IN MOTION refutes it even if the parked image improves, because a flicker ")
		TEXT("is a worse defect than the graininess it replaced. JUDGE THIS ON A MOVING CAPTURE. ")
		TEXT("A parked capture cannot show a temporal artefact at all, so a parked A/B is ")
		TEXT("necessary and is not sufficient.\n")
		TEXT("NOT THE ENGINE'S TAA/TSR JITTER, AND NOT A SUBSTITUTE FOR IT. This offset is in ")
		TEXT("full-res PIXELS inside a 2x2 block; the engine's is in NDC and spans +/-0.5 of ONE ")
		TEXT("full-res pixel, so unscaled it reaches only the middle quarter of the block and can ")
		TEXT("never place a sample on the outer pixels' centres -- it cannot do this arm's job. ")
		TEXT("The two are ORTHOGONAL and compose; see voxel.March.TAAJitter, which carries the ")
		TEXT("engine's phase on the RAY.\n")
		TEXT("THE OLD TEXT HERE CLAIMED 'there is no phase to align to' BECAUSE THE RAY IS BUILT ")
		TEXT("FROM THE PROJECTION DIAGONAL WHILE THE JITTER LIVES IN THE THIRD ROW. The premise is ")
		TEXT("right and is a BUG; the conclusion was wrong. With UE's row-vector convention the ")
		TEXT("jitter survives the perspective divide as a constant NDC translation, so it inverts ")
		TEXT("exactly -- see voxel.March.TAAJitter's block for the derivation. Every half-res ")
		TEXT("jitter number in the archive was measured with the RAY UNJITTERED.\n")
		TEXT("What this arm's own alignment was for is avoiding a BEAT, and both cycle lengths are ")
		TEXT("powers of two against r.TemporalAASamples (8), so the combined pattern repeats in 8 ")
		TEXT("frames."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchHalfResJitterPhase(
		TEXT("voxel.March.HalfRes.JitterPhase"), -1,
		TEXT("PIN THE LATTICE TO ONE PHASE. -1 = free-running from the render-thread frame ")
		TEXT("number, which is what a real leg wants. 0..N-1 = hold that phase every frame, ")
		TEXT("which is what a MEASUREMENT wants.\n")
		TEXT("WHY IT EXISTS. voxel.March.VerifyDepth grades the reconstruction at full res, and ")
		TEXT("under a moving lattice its numbers become frame-dependent: a fallback pixel is ")
		TEXT("graded against a sample up to a pixel further away than it was under the static ")
		TEXT("lattice. Pinning makes the gate repeatable, makes a phase-0 against phase-1 A/B a ")
		TEXT("clean image-space demonstration that the lattice really moved, and lets a still ")
		TEXT("capture isolate ONE lattice instead of averaging the cycle.\n")
		TEXT("A PINNED PHASE IS A STATIC LATTICE. It is a measurement tool and never a shipping ")
		TEXT("configuration -- pinned, this arm gives TSR nothing new and reproduces exactly the ")
		TEXT("graininess it was built to remove. Values are taken modulo the scheme's cycle."),
		ECVF_RenderThreadSafe);

	// ---- THE ENGINE'S OWN TAA/TSR JITTER, CARRIED BY THE MARCHED RAY -------
	//
	// THIS IS NOT voxel.March.HalfRes.Jitter AND THE TWO MUST NOT BE CONFLATED.
	// That one moves the HALF-RES SAMPLE inside its 2x2 block, in full-res
	// PIXELS, and its job is to let every full-res pixel be marched once per
	// cycle. This one applies the ENGINE's per-frame sub-pixel offset -- the
	// same one every rasterised primitive in the frame already carries -- to the
	// RAY, in NDC. They are orthogonal: at full res the half-res uniform is
	// forced to zero and this one is the only offset in play.
	//
	// THE DEFECT IT FIXES, and it was asserted to be unfixable three feet above
	// this line. The comment on HalfRes.Jitter says "there is no phase to align
	// to" because "both halves build their rays from the projection DIAGONAL,
	// while a perspective TAA jitter lives in the third row". The FIRST half of
	// that is exactly right and is the bug; the CONCLUSION does not follow.
	//
	// UE writes the jitter into ViewToClip.M[2][0]/M[2][1]
	// (SceneView.h:631-650, HackAddTemporalAAProjectionJitter, perspective
	// branch), in NDC units, Y already negated to clip convention
	// (SceneVisibility.cpp:5450). MarchInvProjDiag is 1/M[0][0], 1/M[1][1], so
	// it is PROVABLY jitter-invariant -- the marched ray is bit-identical
	// whether the frame is jittered or not, while the raster geometry around it
	// moves normally. TSR has been reconstructing terrain from a signal that
	// carries no new spatial information frame to frame.
	//
	// But with UE's row-vector convention and M[2][3] == 1:
	//
	//     ndc.x = (view.x / view.z) * M[0][0] + M[2][0]
	//
	// the jitter SURVIVES the perspective divide as a constant NDC translation.
	// So there IS a phase to align to, and inverting that line gives the whole
	// fix: view.x/view.z = (ndc.x - jitter.x) * (1 / M[0][0]).
	//
	// THE SECOND SYMPTOM, which is the one an image A/B may not show and the
	// depth gate will. VoxelMarch.usf:1961 already projects the hit through the
	// JITTERED TranslatedWorldToClip, deliberately. The x/y jitter does not
	// reach clip.z or clip.w, so the depth VALUE is unaffected -- but the point
	// being projected was found along an UNJITTERED ray, while the raster
	// prepass writes at that same pixel the depth of the surface along the
	// JITTERED ray. The two renderers therefore disagree sub-pixel at every
	// pixel with a depth gradient, which is precisely what voxel.March.VerifyDepth
	// measures. Turning this on should move that gate DOWN, and that is a
	// numeric prediction that can come out the other way.
	//
	// DEFAULTED TO 0, WHICH IS THE CURRENT SHIPPING BEHAVIOUR. The unjittered
	// ray is the control every image and every leg in the archive was taken
	// against, and it stays reachable.
	//
	// THE HONEST COST, so nobody reports only the win: a frozen sample is a
	// STABLE sample. The marcher's parked near-field instability is ~0.0013%
	// against a 0.0124% control-vs-control noise floor -- it had accidentally
	// cured its own shimmer by being frozen, and an aliased-but-still image
	// scores perfectly on a frame-to-frame metric. Expect that number to RISE
	// toward the quad path's documented 0.92% near / 0.43% far
	// (Config/DefaultEngine.ini:39-84). This is a TRADE, not a free win.
	//
	// NOT A PERMUTATION, for the same reason MarchSampleJitter is not: it
	// changes no buffer extent, no dispatch shape and no tile size -- only a
	// float2 uniform that every permutation already binds.
	TAutoConsoleVariable<int32> CVarVoxelMarchTAAJitter(
		TEXT("voxel.March.TAAJitter"), 1,
		TEXT("CARRY THE ENGINE'S TAA/TSR SUB-PIXEL JITTER ON THE MARCHED RAY. DEFAULT 1 SINCE ")
		TEXT("2026-08-26; 0 is the OLD behaviour and is now a measurement arm, not the shipping ")
		TEXT("one: the ray is built from the projection DIAGONAL only, so it is bit-")
		TEXT("identical whether the frame is jittered or not and TSR accumulates a signal with no ")
		TEXT("new spatial information. 1 = subtract the frame's TemporalAAJitter from NDC before ")
		TEXT("the ray is built, so the marched sample lands where TSR believes a sample for that ")
		TEXT("pixel lands -- the same offset every rasterised primitive already carries.\n")
		TEXT("THE PHASE IS READ IN PreRenderBasePass_RenderThread, NOT IN THE VIEW STASH, and that ")
		TEXT("is not a preference. PreRenderView_RenderThread runs from OnRenderBegin, which is ")
		TEXT("BEFORE BeginInitViews adds the jitter to the projection, so GetTemporalAAJitter() ")
		TEXT("there is exactly (0,0) on every frame and this arm would be silently inert. Read the ")
		TEXT("'[voxel-march] taajitter window' line to see which of FREE-RUNNING / PINNED / STATIC ")
		TEXT("AT ZERO the run actually had -- it reports the SETTLED state, not the boot state.\n")
		TEXT("NOT voxel.March.HalfRes.Jitter. That moves the HALF-RES sample inside its 2x2 block, ")
		TEXT("in full-res PIXELS. This is the ENGINE's phase, in NDC, and at full res it is the ")
		TEXT("only offset in play. The two are orthogonal and compose.\n")
		TEXT("BOTH RAY SITES MOVE TOGETHER OR NEITHER DOES. VoxelMarchBuildRay (the march) and ")
		TEXT("VoxelMarchSampleDirWorld (the emit's reconstruction) read this same uniform from the ")
		TEXT("same frame's resolve. If only one moved, the emit would rebuild a different ray than ")
		TEXT("the one the march measured t along -- a sub-voxel depth error on every pixel, which ")
		TEXT("VoxelMarch.usf names as the hardest defect to attribute in this renderer.\n")
		TEXT("THE FALSIFIER, PRE-REGISTERED. INERT: with r.TemporalAA.Debug.OverrideTemporalIndex ")
		TEXT("pinned to two different values, a settled pinned-pose capture must DIFFER under 1 and ")
		TEXT("must be identical (to the control-vs-control noise floor) under 0. If arm 1 is also ")
		TEXT("identical, the uniform never arrived and every image since is of arm 0.\n")
		TEXT("MEASURED 2026-08-26, PINNED POSE (-61440,-61440 / alt 220 m / pitch -12), AND THIS ")
		TEXT("IS WHY THE DEFAULT IS 1. IMAGE: 41.44%% of pixels differ between arm 0 and arm 1 ")
		TEXT("against a 0.0138%% control-vs-control noise floor -- three thousand times the ")
		TEXT("floor, so the uniform provably arrives. Correctly LOCALISED: terrain 54.0%%, sky ")
		TEXT("2.3%%, i.e. it moves the marched surface and leaves the un-marched background ")
		TEXT("alone, which is what a ray-origin offset must do and what a global tint could ")
		TEXT("not.\n")
		TEXT("TIMING: FREE. Alternated A,B,A,B to absorb drift -- p50 8.45 / 8.43 / 8.42 / 8.41 ")
		TEXT("ms. The arm is two subtracts in the ray setup; there is no cost to trade against ")
		TEXT("and no reason to gate it on scalability.\n")
		TEXT("THE COST, WHICH IS REAL AND IS NOT TIME. A frozen sample is a STABLE sample, and ")
		TEXT("the marcher had accidentally cured its own shimmer by being aliased-but-still. ")
		TEXT("Parked instability RISES ~30x, 0.0071%% -> 0.2097%%. That is the honest price and ")
		TEXT("it is paid knowingly. FOR SCALE: the quad path this renderer replaces ships at ")
		TEXT("0.92%% near / 0.43%% far (Config/DefaultEngine.ini:39-84), so arm 1 is still ")
		TEXT("4.4x steadier than the path already in the owner's hands. Report both sides.\n")
		TEXT("WHAT THE PARKED NUMBER CANNOT TELL YOU: shimmer IN MOTION. A parked capture ")
		TEXT("cannot show a temporal artefact at all. The parked A/B is necessary and is not ")
		TEXT("sufficient; the shipping verdict is a moving PIE session."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchHTileProbe(
		TEXT("voxel.March.HTileProbe"), 0,
		TEXT("THE ONLY WAY TO MEASURE THE HTILE BILL, and it needs voxel.March 2. ")
		TEXT("EmitGpuMs is NOT that bill -- it is the cost of the emit pass itself. The 0.2-0.4 ms ")
		TEXT("the plan budgets is paid by everything downstream that reads depth (RenderLights, ")
		TEXT("SSAO, translucency, TSR) after SV_Depth invalidated the depth metadata, and none of ")
		TEXT("that is inside any bracket this module owns.\n")
		TEXT("Mode 2 on its own CANNOT see it: the emit writes a scratch depth buffer, so the real ")
		TEXT("SceneDepth's HTILE is never touched and the bill is exactly zero while the leg looks ")
		TEXT("like it measured something. 1 = point the emit's DEPTH-STENCIL binding at the REAL ")
		TEXT("SceneDepth while colour still goes to scratch. Probe-on minus probe-off, same pose, ")
		TEXT("differs ONLY in whether the real depth's HTILE survived.\n")
		TEXT("IT CORRUPTS THE IMAGE BY CONSTRUCTION -- real depth, raster colour, terrain lit at ")
		TEXT("the wrong depth. TIMING ARM ONLY. Never screenshot it, never run the depth gate with ")
		TEXT("it on (it is refused there), and read the frame time, not EmitGpuMs.\n")
		TEXT("AS OF 2026-08-24 THIS HAS NEVER BEEN SET TO 1 ON A SINGLE LEG. Grep Saved/ for ")
		TEXT("htileProbe=1 -- there are no hits. So the 0.2-0.4 ms in ")
		TEXT("docs/ray-marching-plan-2026-08-19.md:932 is a budget line that has never been a ")
		TEXT("reading, and nothing should be built to reclaim it until it has been.\n")
		TEXT("AND WHEN IT HAS: the bill has two halves and only one of them is addressable. ")
		TEXT("Per-pixel SV_Depth breaks plane-equation depth compression outright, and no ")
		TEXT("conservative-depth declaration restores it. What a conservative declaration can ")
		TEXT("preserve is one side of the HiZ z-range -- and the side that keeps a DOWNSTREAM ")
		TEXT("reader able to reject is the LOWER bound under reverse-Z (SV_DepthGreaterEqual), ")
		TEXT("which is the OPPOSITE of the upper bound (SV_DepthLessEqual) that would buy early-Z ")
		TEXT("on the emit itself. One flag cannot be both. The early-Z half is separately settled ")
		TEXT("and it is settled NO -- see CONSERVATIVE DEPTH: THE PRIZE IS STRUCTURALLY ZERO in ")
		TEXT("VoxelMarch.usf, which shows the t_max clamp has already done that culling."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchSettleFrames(
		TEXT("voxel.March.SettleFrames"), 60,
		TEXT("How many consecutive frames the chunk index must go WITHOUT AN UPLOAD before ")
		TEXT("voxel.March.VerifySource will sample. 0 disables the wait.\n")
		TEXT("WHY IT EXISTS: after the frame fix made the rays identical (stepsFlat agreeing to ")
		TEXT("0.017%% across byte-identical runs), `lost` still swung 18x while `gained` and ")
		TEXT("`moved` went tight. Both walks run in ONE kernel against ONE set of SRVs, so they ")
		TEXT("cannot see different worlds within a leg -- what differs BETWEEN legs is which ")
		TEXT("chunks are resident, and a geometry-specific defect is dominated by whether the ")
		TEXT("few chunks that trigger it happen to have arrived.\n")
		TEXT("Every other gate on this project learned this separately: the byte-equality gate ")
		TEXT("reads only a converged pool, the depth gate needs a settled scene. A comparator ")
		TEXT("that samples a volume still accepting chunks is measuring the streaming schedule.\n")
		TEXT("The refusal is LOUD and says how long it has been waiting, because a gate that ")
		TEXT("silently never runs is indistinguishable from one that ran and found nothing."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchMutateCounters(
		TEXT("voxel.March.MutateCounters"), 0,
		TEXT("MUTATION ARM FOR THE CONSERVATION CHECK. 1 = the comparator deliberately adds one ")
		TEXT("`lost` that no ray produced, so the split sums no longer match their parent. ")
		TEXT("A CHECK THAT HAS NEVER FAILED IS NOT YET KNOWN TO BE A CHECK. The conservation test ")
		TEXT("has passed on every leg -- including one that looked arithmetically impossible -- ")
		TEXT("which is exactly when a reader starts relying on it. Run this once: if COUNTER ")
		TEXT("CONSERVATION FAILED does not appear, the check is decorative and every number it ")
		TEXT("has blessed is unverified. ")
		TEXT("The corruption is applied in the SHADER, so the arm exercises slot layout, readback ")
		TEXT("and decode alignment rather than only the C++ arithmetic. NEVER leave it on: it ")
		TEXT("makes the lost counters wrong by one on purpose."),
		ECVF_RenderThreadSafe);

	// THE WHOLE-GRID INDEX HASH (56 MiB -- the "4 MiB" this comment used to say
	// was the old one-level kDimZ=64 grid), AS AN A/B SWITCH RATHER THAN A REBUILD.
	//
	// FVoxelMarchChunkIndex::MarkDirtyAndUpload used to run an FNV-1a over the
	// whole chunk-index grid on the GAME THREAD, once per pool flush,
	// unconditionally -- measured at 3,146-3,190 ms per 5 s window against a
	// streaming tick totalling ~3,700 ms. It is now off unless something asks
	// for it, and the only thing that asks is the source comparator below.
	//
	// This cvar exists so the cost can be A/B'd IN ONE BINARY. Gating it purely
	// in code meant the before/after comparison spanned two builds, and the
	// first thing that comparison produced was a frame-time move nobody could
	// attribute -- exactly the two-legs-two-binaries problem this project keeps
	// paying for. 1 forces the hash on with the comparator off.
	TAutoConsoleVariable<int32> CVarVoxelMarchIndexContentHash(
		TEXT("voxel.March.IndexContentHash"), 0,
		TEXT("Force the chunk index's whole-grid (56 MiB) FNV content hash on (1) with the source comparator "
		     "off. Default 0. The hash is otherwise enabled only by voxel.March.VerifySource, "
		     "which is the only consumer of the value. MEASUREMENT SWITCH: it costs ~85% of the "
		     "streaming tick, so this is how you A/B that cost in a single binary rather than "
		     "across two builds."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchVerifySource(
		TEXT("voxel.March.VerifySource"), 0,
		TEXT("THE SOURCE COMPARATOR. Needs voxel.March.Source 1. Marches BOTH volumes over the ")
		TEXT("SAME rays in the SAME frame and compares (hit, voxel, t) per pixel -- which is the ")
		TEXT("only thing that separates a TRAVERSAL regression from a SOURCE regression. ")
		TEXT("A comparison across two dispatches would compare two camera positions as well as ")
		TEXT("two volumes; across two legs, two worlds.\n")
		TEXT("READ IT AGAINST refNoise, NEVER AGAINST ZERO. The comparator also runs the ")
		TEXT("occupancy walk a second time over a segment scaled by 1-1e-7 -- geometrically the ")
		TEXT("same ray -- and counts how often the reference disagrees with ITSELF. Phase 0 ")
		TEXT("established that discipline the hard way: 20 mismatches in 1.35M rays looked like ")
		TEXT("a defect and were the reference's own float32 ordering noise.\n")
		TEXT("TWO NAMED EXCLUSIONS, both decided in advance. MAT_WATERMARK: the occupancy volume ")
		TEXT("packs with isSolidForFluid which EXCLUDES it, the render predicate is m != MAT_AIR ")
		TEXT("which includes it, so a brick-only hit on a watermark voxel is CORRECT -- counted ")
		TEXT("in its own column, never absorbed into a tolerance. Shared-face ties: adjacent ")
		TEXT("voxels, both solid, same point on the ray, which raycast.h's tie rule does not ")
		TEXT("arbitrate and which have no canonical answer.\n")
		TEXT("AND THE FIELD THAT LOCALISES: for every ray where occupancy found a surface and ")
		TEXT("the brick walk did not, it asks the brick path directly about THAT voxel and ")
		TEXT("reports the BRANCH that rejected it. reason=solid means the pool holds that voxel ")
		TEXT("as solid and the walk never tested it -- the DDA is skipping. A count cannot ")
		TEXT("separate that from 'the storage says air'; the printed samples can."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchVerifyIndex(
		TEXT("voxel.March.VerifyIndex"), 0,
		TEXT("THE INDEX/RECORD JOIN PROBE. Needs voxel.March.Source 1. Traces NO RAYS: it walks ")
		TEXT("the 16x16x16 chunk box the occupancy window spans and asks, per cell, whether the ")
		TEXT("chunk index and the 32 B record agree -- which isolates the join from the ")
		TEXT("traversal completely.\n")
		TEXT("Built because the first brick-pool A/B hit 1,138 tiles where the occupancy source ")
		TEXT("hit 8,016 while costing 20.6%% MORE. A miss runs to the full step budget and a hit ")
		TEXT("terminates early, so 'hits far less, costs more' is the signature of rays failing ")
		TEXT("to find terrain that is there. It cannot be a legitimate source difference either: ")
		TEXT("m != MAT_AIR is a SUPERSET of the fluid predicate, so the brick volume must mark ")
		TEXT("MORE solid, never less.\n")
		TEXT("Reports tallies AND SAMPLES -- the wanted and recorded coordinates side by side, ")
		TEXT("because a factor of 32 is a units bug, a permutation is the transposition the ")
		TEXT("format doc warns about, and an unrelated value is a stale slot. Read it with ")
		TEXT("voxel.March.Stats. One dispatch of 4,096 threads, outside every timing bracket."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchVerifyDepth(
		TEXT("voxel.March.VerifyDepth"), 0,
		TEXT("THE GATE P3 OWES (plan section 10, gate 3). Per-pixel depth diff, marcher against ")
		TEXT("the raster path, in the frame, at this pose. 0 = off. ANY OTHER VALUE IS A FRAME ")
		TEXT("COUNT: the gate sums that many sampled frames before publishing a result, clamped ")
		TEXT("to [1, 4096], and 64 is a good default.\n")
		TEXT("WHY A WINDOW AND NOT ONE FRAME. The GPU buffer is cleared every frame and the ")
		TEXT("readback ring hands back whichever frame had a slot free, so a single-frame result ")
		TEXT("was one arbitrary frame of a moving scene -- two consecutive reads differed and ")
		TEXT("there was no way to tell a drifting result from a settled one. A fixed window ")
		TEXT("converges and two reads of a settled system are comparable. The published result ")
		TEXT("carries the frame count it covers.\n")
		TEXT("THE VERDICT IS RELATIVE TO A CONTROL, NOT TO ZERO. The gate also compares the ")
		TEXT("RASTER DEPTH against a one-pixel-displaced copy of ITSELF, on the same pixels, ")
		TEXT("through the same threshold and edge mask -- so both sides are the reference. That ")
		TEXT("is what this instrument scores when the marcher is not involved, and it is the ")
		TEXT("denominator the first two runs were missing. One pixel is coarser than the ")
		TEXT("sub-pixel difference actually separating the two renderers, so the control is an ")
		TEXT("UPPER BOUND: a marcher rate above it is a hard result, a rate below it is ")
		TEXT("consistent with sampling noise and is NOT proof of correctness.\n")
		TEXT("REQUIRES voxel.March 2 with voxel.March.HTileProbe 0. Anything that lets the emit ")
		TEXT("write the real SceneDepth turns this into the marcher graded against itself, which ")
		TEXT("passes no matter what is wrong; both cases are refused and say so. ")
		TEXT("Read it with voxel.March.Stats. The pass is outside both timing brackets, so it ")
		TEXT("cannot move marchMs or emitMs -- but it makes the GPU busier, so do not quote a ")
		TEXT("timing from a gate run."),
		ECVF_RenderThreadSafe);

	// One-shot complaint for the DBuffer cvar, so an operator who sets it to 1
	// finds out why nothing happened instead of concluding decals are broken.
	bool GVoxelMarchDBufferComplained = false;

	// One-shot latch for voxel.March.TAAJitter's sign self-test. A file-scope
	// bool rather than a function static so the check is re-runnable by nothing
	// -- it is a statement about the engine's matrices, not about a leg, and one
	// PASS per process is the whole of it.
	bool GVoxelMarchTAAJitterSignChecked = false;

	// ---- WHAT THE MARCH LAST ACTUALLY DISPATCHED ---------------------------
	//
	// NOT what the cvar asks for. voxel.March.Stats runs on the game thread and
	// can only read cvars; a cvar reads back whatever was typed, including on
	// frames where the pass declined, and this project has a recorded incident
	// of exactly that shape -- a switch that was armed, printed as armed, and
	// ran the same configuration twice.
	//
	// So the march hook stamps the dimensions it really handed the dispatch, and
	// the stats line prints those. -1 for the shift means the march has not run
	// since this process started, which is a different statement from 0 and is
	// printed as a different word.
	//
	// Written render thread, read game thread. Relaxed atomics rather than plain
	// ints: nothing else is ordered against these -- they are a report, not a
	// handshake -- but a torn read would print a shape that never existed, and
	// this line's entire job is to be believable.
	std::atomic<int32> GVoxelMarchRanSampleWidth{0};
	std::atomic<int32> GVoxelMarchRanSampleHeight{0};
	std::atomic<int32> GVoxelMarchRanViewWidth{0};
	std::atomic<int32> GVoxelMarchRanViewHeight{0};
	std::atomic<int32> GVoxelMarchRanResShift{-1};

	// ---- THE RESIDENT-Z BOUND, AS ACTUALLY UPLOADED (voxel.March.ZCut) -----
	//
	// SAME RAN-FLAG DISCIPLINE AS THE BLOCK ABOVE, and for the same reason: the
	// cvar reads back what was typed, the bind reports what was sent. -1 for
	// the enable means VoxelMarchBindPool has NEVER RUN since this process
	// started, which the stats line prints as a word rather than as an off.
	//
	// UsableMask is the half that catches the interesting failure. Enable 1
	// with UsableMask 0 is "the arm is on and every level refused a bound" --
	// an index that has never seeded, or a run with nothing resident -- and it
	// is a completely different finding from "the arm is on and cut nothing",
	// which reads Enable 1, UsableMask non-zero, zcutSkipped 0.
	//
	// Written render thread, read game thread, relaxed: a report, not a
	// handshake.
	std::atomic<int32> GVoxelMarchZCutRanEnable{-1};
	std::atomic<int32> GVoxelMarchZCutRanUsableMask{0};
	std::atomic<int32> GVoxelMarchZCutRanPad{0};
	std::atomic<int32> GVoxelMarchZCutRanZMin[8] = {};
	std::atomic<int32> GVoxelMarchZCutRanZMax[8] = {};

	// THE HEIGHT PYRAMID'S BIND STAMP, for the reason the Z bound has one: the
	// cvar reads back whatever was typed, and "the cvar says 1" has been
	// mistaken for "the arm ran" enough times in this project that the bind now
	// reports itself. -1 means NO BIND HAS HAPPENED YET, which is a different
	// finding from 0 (bound, and deliberately disabled) and must not print as
	// the same thing.
	//
	// Written render thread, read game thread, relaxed: a report, not a
	// handshake.
	std::atomic<int32> GVoxelMarchHeightRanEnable{-1};
	std::atomic<int32> GVoxelMarchHeightRanDim{0};
	std::atomic<int32> GVoxelMarchHeightRanLeafLevel{-1};
	std::atomic<int32> GVoxelMarchHeightRanMips{0};

	// ---- THE HALF-RES LATTICE'S PER-FRAME OFFSET ---------------------------
	//
	// ONE TABLE, ONE RESOLVE, READ BY FOUR PASSES. The march, the depth
	// pre-emit, the GBuffer emit and the verify gate must all place the sample
	// at the SAME point, because the reconstruction intersects this pixel's ray
	// with a plane derived from the CANDIDATE's ray -- and if the two halves
	// disagree about where the candidate stood, the plane is not the plane the
	// march crossed. The failure is a sub-voxel depth error spread over the
	// whole image, which is the hardest defect to attribute in this renderer.
	// So the offset is a pure function of (scheme, phase) and nobody computes a
	// second version of it.
	//
	// IN FULL-RES PIXELS, applied INSIDE the 2x2 block, so the sample stays in
	// its own block at every phase and 'the containing sample' keeps meaning
	// what VoxelMarchReconstruct's Base index says it means.

	// Cycle length per scheme. Both are POWERS OF TWO, which is the whole of the
	// answer to "why not align to the engine's TAA phase": against
	// r.TemporalAASamples (8 by default) the least common multiple is 8, so the
	// combined pattern repeats in eight frames instead of beating slowly, and a
	// slow beat is a worse artefact than the static graininess this replaces.
	constexpr int32 kVoxelMarchJitterCycle[3] = { 1, 4, 8 };

	int32 VoxelMarchJitterCycleLength(int32 Scheme)
	{
		return kVoxelMarchJitterCycle[FMath::Clamp(Scheme, 0, 2)];
	}

	// SCHEME 1 -- the four full-res pixel CENTRES, in an order whose consecutive
	// frames are diagonally opposite and then adjacent, so no two-frame swing
	// dominates and there is no left-right shuffle to read as a wobble.
	//
	// Its property, and the reason it is the one to try first: over one cycle
	// EVERY full-res pixel is marched exactly once AT ITS OWN CENTRE, so TSR has
	// seen the ground truth for every pixel within four frames. That is plain
	// checkerboard rendering, with this arm's exact-depth reconstruction filling
	// the other three frames instead of a filter.
	//
	// VoxelMarch.usf's VoxelMarchSampleSvPosition refuses a STATIC corner sample
	// on the ground that it biases the fallback population toward one corner of
	// every block, which is a direction-dependent artefact. The rotation removes
	// the premise rather than overruling it: the bias exists in any one frame
	// and has no direction at all over the cycle.
	constexpr int32 kVoxelMarchJitterHaltonCount = 8;

	FVector2f VoxelMarchJitterOffset(int32 Scheme, int32 Phase)
	{
		static const FVector2f Quad[4] = {
			FVector2f(-0.5f, -0.5f), FVector2f(+0.5f, +0.5f),
			FVector2f(+0.5f, -0.5f), FVector2f(-0.5f, +0.5f),
		};
		// SCHEME 2 -- Halton(2,3), the first eight terms, mapped from [0,1) onto
		// the same +/-0.5 box. Eight distinct positions instead of four and none
		// of them a pixel centre, so no pixel is ever exact by construction --
		// which is the trade against scheme 1 and is why scheme 1 is the
		// default when on. It exists because if scheme 1 shows visible
		// FOUR-FRAME structure (a rotating artefact rather than a static one)
		// this is the thing to try, and the box is shared between sessions, so
		// a second build costs a day.
		static const FVector2f Halton[kVoxelMarchJitterHaltonCount] = {
			FVector2f(0.5000f - 0.5f, 0.3333f - 0.5f),
			FVector2f(0.2500f - 0.5f, 0.6667f - 0.5f),
			FVector2f(0.7500f - 0.5f, 0.1111f - 0.5f),
			FVector2f(0.1250f - 0.5f, 0.4444f - 0.5f),
			FVector2f(0.6250f - 0.5f, 0.7778f - 0.5f),
			FVector2f(0.3750f - 0.5f, 0.2222f - 0.5f),
			FVector2f(0.8750f - 0.5f, 0.5556f - 0.5f),
			FVector2f(0.0625f - 0.5f, 0.8889f - 0.5f),
		};
		if (Scheme == 1) { return Quad[Phase & 3]; }
		if (Scheme == 2) { return Halton[Phase & 7]; }
		return FVector2f::ZeroVector;
	}

	struct FVoxelMarchJitter
	{
		int32     Scheme = 0;
		int32     Phase = 0;
		int32     Cycle = 1;
		FVector2f Offset = FVector2f::ZeroVector;
	};

	// THE ONE RESOLVE. bHalfRes gates it because a jittered FULL-res march is
	// not a softer picture, it is a wrong one: at full res the emit does a
	// direct Load at the pixel's own texel and rebuilds its ray from its own
	// unjittered SV_Position, so a moved march ray would hand it a t measured
	// along a different ray for every pixel on screen. VoxelMarch.usf guards the
	// same thing with an #if; neither half can arm this alone.
	FVoxelMarchJitter VoxelMarchResolveJitter(bool bHalfRes, uint32 FrameNumber)
	{
		FVoxelMarchJitter J;
		if (!bHalfRes)
		{
			return J;
		}
		J.Scheme = FMath::Clamp(CVarVoxelMarchHalfResJitter.GetValueOnRenderThread(), 0, 2);
		if (J.Scheme == 0)
		{
			return J;
		}
		J.Cycle = VoxelMarchJitterCycleLength(J.Scheme);
		// PINNED IS A MEASUREMENT MODE AND A PINNED LATTICE IS A STATIC ONE --
		// see the cvar's own help. It is here so the verify gate is repeatable
		// and so a phase-0 against phase-1 still capture demonstrates in image
		// space that the lattice really moves.
		const int32 Pinned = CVarVoxelMarchHalfResJitterPhase.GetValueOnRenderThread();
		J.Phase = (Pinned >= 0) ? (Pinned % J.Cycle)
		                        : int32(FrameNumber % uint32(J.Cycle));
		J.Offset = VoxelMarchJitterOffset(J.Scheme, J.Phase);
		return J;
	}

	// ---- WHAT THE MARCH LAST ACTUALLY JITTERED BY --------------------------
	//
	// Same discipline as the Ran* block above and for the same recorded reason:
	// a cvar reads back what was TYPED. These are stamped by the march hook from
	// the value it handed the dispatch, and -1 means the march has not run since
	// this process started, which is a different statement from 0.
	//
	// StampFrame is the JOIN, and it is CHECKED rather than derived. The emit
	// and the gate run later in the same frame and re-derive the phase from
	// their own frame number; if the derived answer disagrees with the stamp,
	// something moved the lattice between the march and the emit and the
	// reconstruction is about to intersect a plane the march never crossed.
	// Five bugs in this project in three days shared exactly that shape -- a
	// join COMPUTED instead of CHECKED.
	//
	// PhaseMask is the proof of traffic: one bit per phase dispatched since the
	// scheme last changed. Its popcount is 'phasesSeen', and phasesSeen=1 on a
	// settled leg is the reading that says this arm is armed and inert.
	std::atomic<uint32> GVoxelMarchJitterStampFrame{0u};
	std::atomic<int32>  GVoxelMarchRanJitterScheme{-1};
	std::atomic<int32>  GVoxelMarchRanJitterPhase{-1};
	std::atomic<int32>  GVoxelMarchRanJitterCycle{0};
	std::atomic<uint32> GVoxelMarchRanJitterPhaseMask{0u};
	std::atomic<uint64> GVoxelMarchRanJitterFrames{0};

	// THE EMIT AND GATE SIDE OF THE JOIN. Returns the offset the MARCH actually
	// used, and CHECKS it against the phase this hook would have derived on its
	// own.
	//
	// THE STAMP IS THE AUTHORITY AND THE DERIVATION IS THE CHECK, in that order
	// and not the other way round. The reconstruction intersects this pixel's
	// ray with a plane taken from a CANDIDATE's ray, so it must stand where the
	// march stood; only the march knows that. The two can differ solely because
	// voxel.March.HalfRes.Jitter moved between the two hooks of one frame, which
	// is one frame of settling and no longer -- and it is reported rather than
	// absorbed, because a join computed instead of checked is the shape five
	// bugs in this project shared in three days.
	FVector2f VoxelMarchJitterForEmit(bool bHalfResEmit, uint32 FrameNumber)
	{
		if (!bHalfResEmit)
		{
			// FULL RES IS ALWAYS ZERO. The emit loads the pixel's own texel here
			// and rebuilds its ray from its own unjittered SV_Position; an
			// offset would make those two describe different rays.
			return FVector2f::ZeroVector;
		}
		const uint32 StampFrame = GVoxelMarchJitterStampFrame.load(std::memory_order_relaxed);
		const int32 Scheme = GVoxelMarchRanJitterScheme.load(std::memory_order_relaxed);
		const int32 Phase = GVoxelMarchRanJitterPhase.load(std::memory_order_relaxed);
		if (StampFrame != FrameNumber || Scheme < 0)
		{
			static bool bStampComplained = false;
			if (!bStampComplained)
			{
				bStampComplained = true;
				UE_LOG(LogVoxelMarch, Error,
				       TEXT("Voxel march emit: the half-res lattice stamp is from frame %u and "
				            "this hook is on frame %u (scheme %d). The emit cannot know where the "
				            "march stood, so it is falling back to the STATIC lattice -- which "
				            "means the reconstruction is about to intersect planes the march did "
				            "not cross, and the symptom is terrain at a sub-voxel wrong depth "
				            "with no other error. This should be unreachable: the emit already "
				            "requires the view to have marched this frame."),
				       StampFrame, FrameNumber, Scheme);
			}
			return FVector2f::ZeroVector;
		}
		const FVoxelMarchJitter Derived = VoxelMarchResolveJitter(true, FrameNumber);
		if (Derived.Scheme != Scheme || Derived.Phase != Phase)
		{
			static bool bDriftWarned = false;
			if (!bDriftWarned)
			{
				bDriftWarned = true;
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("Voxel march emit: the march ran lattice scheme %d phase %d and this "
				            "hook derives scheme %d phase %d for the same frame. ONE frame of "
				            "this is a live voxel.March.HalfRes.Jitter change settling. Longer "
				            "means the two hooks are reading different switches, and every image "
				            "taken since has a reconstruction built on the wrong sample point. "
				            "The emit is using the MARCH's values, which are the correct ones."),
				       Scheme, Phase, Derived.Scheme, Derived.Phase);
			}
		}
		return VoxelMarchJitterOffset(Scheme, Phase);
	}
}

FVoxelMarchArm VoxelMarchGetArm()
{
	FVoxelMarchArm Arm;
	Arm.Mode = FMath::Clamp(CVarVoxelMarch.GetValueOnAnyThread(), 0, 2);
	Arm.StepBudget = FMath::Clamp(CVarVoxelMarchStepBudget.GetValueOnAnyThread(), 1, 4096);
	Arm.Source = FMath::Clamp(CVarVoxelMarchSource.GetValueOnAnyThread(), 0, 1);
	Arm.SkipLevels = FMath::Clamp(CVarVoxelMarchSkipLevels.GetValueOnAnyThread(), 0, 2);
	// Rings are meaningless against the occupancy source -- one flat volume, no
	// levels -- and the permutation refuses to compile that combination, so the
	// arm must not ask for it either.
	Arm.bRings = (Arm.Source == 1) && (CVarVoxelMarchRings.GetValueOnAnyThread() != 0);
	// Only the ring walk reads the fallthrough define, so without rings the
	// dimension is forced to its control value -- the permutation for
	// fallthrough > 0 with rings off is refused at compile and must not be
	// asked for here either.
	Arm.Fallthrough =
		Arm.bRings ? FMath::Clamp(CVarVoxelMarchFallthrough.GetValueOnAnyThread(), 0, 2) : 0;
	// Both hole counters are properties of the ring walk (`substituted` needs
	// a segment/level split to compare; `uncovered` needs per-level residency),
	// so without rings the dimension is forced to its control value -- the
	// permutation for hole stats without rings is refused at compile and must
	// not be asked for here either.
	// Clamped to the three permutation values that exist: 1 the cheap
	// certified counters, 2 adds the per-level/per-reason uncovered breakdown.
	// voxel.GpuStream.Prototype passes larger values through; clamping here
	// (rather than rejecting) keeps that one-switch arm working.
	Arm.HoleStatsLevel =
		Arm.bRings ? FMath::Clamp(CVarVoxelMarchHoleStats.GetValueOnAnyThread(), 0, 2) : 0;
	Arm.bHoleStats = Arm.HoleStatsLevel != 0;
	// The coarse occupancy level. RINGS ONLY, for the reason the fallthrough
	// depth is: it is the RING walk's chunk loop that consults it, the
	// permutation for block skip without rings is refused at compile, and an arm
	// that asks for a permutation that does not exist gets whatever
	// TShaderMapRef falls back to -- which is a silently different kernel.
	Arm.bBlockSkip = Arm.bRings && (CVarVoxelMarchBlockSkip.GetValueOnAnyThread() != 0);
	// FORCED OFF WITHOUT A LADDER TO GATE, matching the refused permutation, so
	// the reported arm can never claim a gate the kernel has no code for.
	Arm.bSkyLadder = Arm.bRings && Arm.Fallthrough > 0 &&
	                 (CVarVoxelMarchSkyLadder.GetValueOnAnyThread() != 0);
	// THE PER-RAY RESIDENT-EXTENT BOUND. Rings only (the clamp lives in the
	// ring walk's ZCut socket), and FORCED OFF WHILE THE SKY LADDER IS ARMED:
	// the Bound+SkyLadder permutation is refused at compile (the ladder's
	// gate reads bWalkTruncated, which the bound eliminates -- see the
	// #error in VoxelMarchBound.ush), and an arm that asks for a permutation
	// that does not exist gets whatever TShaderMapRef falls back to. The
	// ladder wins the conflict because it is the arm that changes what is
	// drawn; the render site logs the suppression so a leg cannot read a
	// bound-off frame as a bound null result. HalfRes is the third refused
	// pairing and is enforced at the render site, where ResShift lives.
	Arm.Bound = (Arm.bRings && !Arm.bSkyLadder &&
	             CVarVoxelMarchBound.GetValueOnAnyThread() != 0)
	                ? 1
	                : 0;
	// CLAMPED AND FORCED TO 0 WITHOUT THE PERMUTATION, so the reported mode can
	// never claim an arm the kernel does not contain -- the same rule
	// bBlockSkipArmed follows, one layer up.
	Arm.BlockSkyMode = Arm.bBlockSkip
	                       ? FMath::Clamp(CVarVoxelMarchBlockSkySkip.GetValueOnAnyThread(), 0, 2)
	                       : 0;
	Arm.ReachM = FMath::Max(CVarVoxelMarchReachM.GetValueOnAnyThread(), 0.0f);
	Arm.bAO = CVarVoxelMarchAO.GetValueOnAnyThread() != 0;
	Arm.bVelocity = CVarVoxelMarchVelocity.GetValueOnAnyThread() != 0;
	// Deliberately forced off regardless of the cvar -- see the cvar's text and
	// THE DBUFFER HOLE in VoxelMarch.usf. When the binding exists this becomes
	// `CVarVoxelMarchDBuffer.GetValueOnAnyThread() != 0` and nothing else here
	// changes.
	Arm.bDBuffer = false;
	if (!Arm.bDBuffer && CVarVoxelMarchDBuffer.GetValueOnAnyThread() != 0 &&
	    !GVoxelMarchDBufferComplained)
	{
		GVoxelMarchDBufferComplained = true;
		UE_LOG(LogVoxelMarch, Warning,
		       TEXT("voxel.March.DBuffer 1 was requested and is being IGNORED. The DBuffer "
		            "textures are Renderer-private in UE 5.8 (Renderer/Private/DBufferTextures.h) "
		            "and no public accessor reaches them at PostRenderBasePassDeferred. Decals "
		            "will not land on marched terrain. This is a known gap, recorded in "
		            "docs/ray-marching-plan-2026-08-19.md section 5."));
	}
	return Arm;
}

// ---------------------------------------------------------------------------
// The process-wide hookup
// ---------------------------------------------------------------------------
//
// See the header, section 6. One publisher, called from the game thread beside
// the line that already publishes the same pointer to the fluid renderer. It
// owns the extension's lifetime so that nothing outside this workstream has to
// know when to create it or when to let it go.
namespace
{
	TSharedPtr<FVoxelMarchState, ESPMode::ThreadSafe> GMarchState;
	TSharedPtr<FVoxelMarchRenderExtension, ESPMode::ThreadSafe> GMarchExtension;
}

namespace
{
	// WHICH SOURCE ACTUALLY NEEDS THE OCCUPANCY VOLUME, and it is only source 0.
	//
	// Source 1 walks the brick pool: VoxelMarchBindPool touches only the pool and
	// the chunk index, and it overwrites the volume-derived frame origin outright.
	// The volume was nonetheless REQUIRED to exist on every path, which coupled
	// terrain rendering to the water simulation running -- the wire is marked
	// temporary at VoxelFluidSubsystem.cpp:2360-2368 ("Until the brick pool
	// lands"), and the brick pool has landed.
	bool VoxelMarchNeedsOccupancyVolume(const FVoxelMarchArm& Arm)
	{
		return Arm.Source == 0;
	}
}

void VoxelMarchEnsureExtension(UWorld* World)
{
	check(IsInGameThread());
	if (World == nullptr)
	{
		return;
	}

	// APPLIED HERE, ON THE GAME THREAD, EVERY TICK -- and that placement is the
	// point. MarkDirtyAndUpload reads this flag from the game thread during a
	// pool flush; the comparator's own enable at the source-compare site runs on
	// the RENDER thread. Setting a flag from one thread that the other reads
	// every flush is a race on a value that decides whether ~85% of the
	// streaming tick happens, so the cvar half is applied from the side that
	// consumes it. The comparator's render-thread enable is left in place: it
	// only ever turns the flag ON, and it turns it on for the one consumer that
	// genuinely needs the value.
	if (CVarVoxelMarchIndexContentHash.GetValueOnGameThread() != 0)
	{
		GetGlobalVoxelMarchChunkIndex().SetContentHashEnabled(true);
	}
	if (!GMarchExtension.IsValid())
	{
		GMarchState = MakeShared<FVoxelMarchState, ESPMode::ThreadSafe>();
		GMarchExtension =
			FSceneViewExtensions::NewExtension<FVoxelMarchRenderExtension>(World, GMarchState);
		UE_LOG(LogVoxelMarch, Display,
		       TEXT("Voxel march view extension registered (classify+march at PreRenderBasePass, "
		            "SV_Depth/GBuffer emit at PostRenderBasePassDeferred). voxel.March 0 = off."));
	}

	// ATTACH THE CHUNK INDEX TO THE POOL. Idempotent, game thread, seeds itself
	// from the resident set on the first call.
	//
	// KEPT HERE, BUT IT IS NO LONGER THE ONLY ATTACH AND WAS NEVER THE ONLY ONE
	// THAT MATTERED. UVoxelShadowMarchSubsystem::OnWorldBeginPlay
	// (VoxelShadowMarch.cpp:1267) already attaches with no fluid dependency in
	// every Game/PIE world, and says so. So "no fluids means the index never
	// attached" is FALSE for a real session -- what a fluid-free run actually
	// lost was the EXTENSION ITSELF, created above -- which is why this function
	// exists and why UVoxelWorldSubsystem, the pool's owner, drives it. The call
	// stays here because it is free when already attached and because a source
	// flip must not start tracking late.
	GetGlobalVoxelMarchChunkIndex().AttachToGlobalPool();
}

void VoxelMarchReleaseExtension()
{
	check(IsInGameThread());

	// Reset in the same order UVoxelShadowMarchSubsystem::Deinitialize uses --
	// extension first, then the state it holds -- so the extension is off the
	// engine's list before the state it points at goes away.
	//
	// Dropping these is what lets EnsureExtension build a NEW extension bound
	// to the NEXT world. Without it the IsValid() guard above is satisfied
	// forever by a pointer to a DEAD world, and the marcher silently never
	// registers again: terrain streams normally and nothing draws it, while
	// water keeps rendering through its own quad pools. See the header.
	if (GMarchExtension.IsValid() || GMarchState.IsValid())
	{
		UE_LOG(LogVoxelMarch, Display,
		       TEXT("Voxel march view extension released (world teardown). The next Game/PIE "
		            "world builds a fresh one; without this it would silently reuse a dead one."));
	}
	GMarchExtension.Reset();
	GMarchState.Reset();
}

void VoxelMarchPublishSource(UWorld* World,
                             const TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe>& InVolume)
{
	check(IsInGameThread());
	VoxelMarchEnsureExtension(World);
	if (GMarchState.IsValid())
	{
		FScopeLock Guard(&GMarchState->Lock);
		GMarchState->Volume = InVolume;
	}

}

// ---------------------------------------------------------------------------
// THE READOUT
// ---------------------------------------------------------------------------
//
// `voxel.March.Stats`. A command and not a 1 Hz log line, because this module
// has no tick of its own and borrowing the fluid subsystem's perf line would be
// a second edit to a file this workstream does not own.
//
// EVERY FIELD IS PRINTED AS A WORD WHEN IT IS NOT A NUMBER. "pending" is no GPU
// timing has ever landed; "never-ran" is the pass was never added; "off" is the
// cvar is 0. All three are distinct from a real 0.00, and the whole reason they
// are is that a marcher which silently never ran must never be able to print a
// small number and be believed.
//
// READ marchMs AND emitFrames TOGETHER. If emitFrames is behind frames, the emit
// declined and RDG CULLED THE MARCH -- its outputs had no consumer -- so marchMs
// describes a pass whose work was thrown away. The timing brackets are
// NeverCull, so they still report; the dispatch between them did not run. That
// is the one way this instrument can print a plausible small number, and the
// counter pair is what exposes it.
static FAutoConsoleCommand GVoxelMarchStatsCmd(
	TEXT("voxel.March.Stats"),
	TEXT("Print the ray-march renderer's GPU times, tile counts and decline reasons."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		const FVoxelMarchArm Arm = VoxelMarchGetArm();
		const FVoxelMarchStats S = VoxelMarchGetStats();
		const auto Ms = [](float V, uint64 Frames) -> FString
		{
			if (Frames == 0) { return TEXT("never-ran"); }
			return V < 0.0f ? FString(TEXT("pending")) : FString::Printf(TEXT("%.3f"), V);
		};
		UE_LOG(LogVoxelMarch, Display,
		       TEXT("Voxel march: mode=%s source=%s stepBudget=%d ao=%d dbuffer=%d "
		            "velocity=%d htileProbe=%d | marchMs=%s emitMs=%s scratchMs=%s | "
		            "frames=%llu emitFrames=%llu | tiles total=%u drawn=%u | indexEntries=%d | "
		            "declined noView=%llu noVolume=%llu noTextures=%llu nonPrimary=%llu unsupported=%llu "
		            "noPool=%llu"),
		       Arm.Mode == 0 ? TEXT("off") : (Arm.Mode == 1 ? TEXT("scene") : TEXT("scratch")),
		       Arm.Source == 0 ? TEXT("occupancy") : TEXT("brickpool"),
		       Arm.StepBudget, Arm.bAO ? 1 : 0, Arm.bDBuffer ? 1 : 0, Arm.bVelocity ? 1 : 0,
		       S.bHTileProbe ? 1 : 0,
		       *Ms(S.MarchGpuMs, S.Frames), *Ms(S.EmitGpuMs, S.EmitFrames),
		       *Ms(S.ScratchGpuMs, S.ScratchFrames), S.Frames,
		       S.EmitFrames, S.TilesTotal, S.TilesHit, S.IndexEntries,
		       S.DeclinedNoView, S.DeclinedNoVolume, S.DeclinedNoTextures,
		       S.DeclinedNonPrimary,
		       S.DeclinedUnsupported, S.DeclinedNoPool);
		// ---- HALF RES: WHAT WAS ASKED FOR, AND WHAT ACTUALLY RAN -----------
		//
		// TWO FIELDS, NOT ONE, and they are printed side by side on purpose. A
		// cvar reads back whatever was typed -- including on a frame where the
		// pass declined, or before the first march of the session -- so "asked"
		// alone is exactly the reading that let nine switches in this project sit
		// armed and inert. "ran" is stamped by the march hook from the dimensions
		// it handed the dispatch, and "never-ran" is a WORD rather than a
		// plausible zero.
		//
		// asked != ran on a settled leg is a defect, not a race: the two can
		// differ for one frame after a live cvar change and no longer than that.
		{
			const int32 RanShift = GVoxelMarchRanResShift.load(std::memory_order_relaxed);
			const int32 Asked = (CVarVoxelMarchHalfRes.GetValueOnAnyThread() != 0) ? 1 : 0;
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("  halfRes: asked=%d ran=%s | rays=%dx%d for view=%dx%d | "
			            "tile=%d full-res px | reconstruction=%s"),
			       Asked,
			       RanShift < 0 ? TEXT("never-ran")
			                    : (RanShift != 0 ? TEXT("1 (half)") : TEXT("0 (full)")),
			       GVoxelMarchRanSampleWidth.load(std::memory_order_relaxed),
			       GVoxelMarchRanSampleHeight.load(std::memory_order_relaxed),
			       GVoxelMarchRanViewWidth.load(std::memory_order_relaxed),
			       GVoxelMarchRanViewHeight.load(std::memory_order_relaxed),
			       kVoxelMarchTileSize << FMath::Max(RanShift, 0),
			       RanShift < 0 ? TEXT("never-ran")
			                    : (RanShift != 0 ? TEXT("exact ray/plane + nearest-neighbour "
			                                            "fallback")
			                                     : TEXT("none (direct Load)")));
			if (RanShift >= 0 && RanShift != Asked)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("  voxel.March.HalfRes asks for %d and the march last RAN %d. One "
				            "frame of this is a live cvar change settling; anything longer means "
				            "the switch is armed and not engaging, and every timing taken since "
				            "describes the other arm."),
				       Asked, RanShift);
			}
		}
		// ---- THE RESIDENT-Z BOUND: ASKED, RAN, AND WHAT IT ACTUALLY CUT ---
		//
		// THREE QUESTIONS, PRINTED SEPARATELY, BECAUSE THEY FAIL SEPARATELY.
		//
		//   1. asked   the cvar. Reads back whatever was typed, on a frame the
		//              pass declined as readily as on one it ran.
		//   2. ran     what VoxelMarchBindPool last UPLOADED, plus which slots
		//              were given a usable bound. "never-ran" is a WORD.
		//              ran=1 usable=0x00 is "on, and every level refused a
		//              bound" -- an index that never seeded, or a world with
		//              nothing resident -- and it is a different finding from
		//              "on and cut nothing".
		//   3. cut     what the GPU did with it. Needs voxel.March.HoleStats,
		//              and says NOT MEASURED, never zero, when that is off.
		//
		// THE ONE READING THAT CONDEMNS THE ARM: asked=1 ran=1 usable non-zero
		// and consulted=0. That is armed-and-inert -- the house failure -- and
		// no frame time taken on such a leg describes this change.
		{
			const int32 Asked = (CVarVoxelMarchZCut.GetValueOnAnyThread() != 0) ? 1 : 0;
			const int32 AskedPad =
				FMath::Max(CVarVoxelMarchZCutPadChunks.GetValueOnAnyThread(), 1);
			const int32 RanEnable = GVoxelMarchZCutRanEnable.load(std::memory_order_relaxed);
			const int32 UsableMask =
				GVoxelMarchZCutRanUsableMask.load(std::memory_order_relaxed);
			const int32 RanPad = GVoxelMarchZCutRanPad.load(std::memory_order_relaxed);
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("  zcut: asked=%d pad=%d | ran=%s usableSlots=0x%02x ranPad=%d"),
			       Asked, AskedPad,
			       RanEnable < 0 ? TEXT("never-ran") : (RanEnable != 0 ? TEXT("1") : TEXT("0")),
			       uint32(UsableMask), RanPad);
			if (RanEnable > 0)
			{
				// THE BOUND ITSELF, PER SLOT, IN CHUNKS AT THAT SLOT'S OWN
				// LATTICE. Printed because "the arm skipped nothing" and "the
				// arm was handed a bound 1,300 chunks tall" are different
				// diagnoses with different owners, and only this line
				// separates them. A slot with no usable bound prints the word
				// rather than the pair it did not receive.
				FString Bounds;
				for (uint32 L = 0; L < FVoxelMarchChunkIndex::kRingGrids; ++L)
				{
					if ((UsableMask & (1 << int32(L))) != 0)
					{
						const int32 Lo =
							GVoxelMarchZCutRanZMin[int32(L)].load(std::memory_order_relaxed);
						const int32 Hi =
							GVoxelMarchZCutRanZMax[int32(L)].load(std::memory_order_relaxed);
						Bounds += FString::Printf(TEXT(" L%u[%d..%d,%d]"), L, Lo, Hi,
						                          Hi - Lo + 1);
					}
					else
					{
						Bounds += FString::Printf(TEXT(" L%u[no-bound]"), L);
					}
				}
				UE_LOG(LogVoxelMarch, Display, TEXT("    padded chunk-Z bound:%s"), *Bounds);
			}
			const FVoxelMarchHoleStats HW = VoxelMarchPeekLastHoleWindow();
			if (!HW.bArmed || HW.Frames == 0)
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    engagement: NOT MEASURED (%s). The uniform above proves the "
				            "DATA reached the shader; only these counters prove the shader "
				            "USED it."),
				       !HW.bArmed ? TEXT("voxel.March.HoleStats is 0")
				                  : TEXT("armed, no readback has landed yet"));
			}
			else
			{
				// consulted - skipped - clipped IS the untouched count, so the
				// three partition every decision and there is no residue to
				// wonder about. Printed as the partition rather than as two
				// rates for exactly that reason.
				const uint64 ZUntouched =
					HW.ZCutConsulted >= (HW.ZCutSkipped + HW.ZCutClipped)
						? HW.ZCutConsulted - HW.ZCutSkipped - HW.ZCutClipped
						: 0;
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    engagement: consulted=%llu = skipped %llu (%.2f%%) + "
				            "clipped %llu (%.2f%%) + untouched %llu, over %llu frames"),
				       (unsigned long long)HW.ZCutConsulted,
				       (unsigned long long)HW.ZCutSkipped,
				       HW.ZCutConsulted > 0
				           ? 100.0 * double(HW.ZCutSkipped) / double(HW.ZCutConsulted)
				           : 0.0,
				       (unsigned long long)HW.ZCutClipped,
				       HW.ZCutConsulted > 0
				           ? 100.0 * double(HW.ZCutClipped) / double(HW.ZCutConsulted)
				           : 0.0,
				       (unsigned long long)ZUntouched,
				       (unsigned long long)HW.Frames);
				// SAID ON THE LINE ITSELF, not only in a header nobody reading a
				// log will open. A skipped share counts DECISIONS; the work it
				// removed was scattered loads whose cost depends on cache state
				// and occupancy, not on how many of them there were. Three
				// published measurements have iterations and wall time moving
				// in OPPOSITE directions, so a reader converting this
				// percentage into an expected millisecond saving is doing
				// something already known to be wrong.
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("      (engagement only -- these are DECISIONS, not nanoseconds. "
				            "The saving is VoxelMarch.March from ProfileGPU and nothing else.)"));
				if (HW.bZCutArmed && HW.ZCutConsulted == 0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    voxel.March.ZCut is 1 and the shader consulted the bound "
					            "ZERO times over %llu measured frames. THE ARM IS ARMED AND "
					            "INERT -- every level refused a bound, the uniform never "
					            "reached this permutation, or the walk never asked. No frame "
					            "time measured on this leg describes the Z bound. FIRST THING "
					            "TO CHECK: this half of the change (the cvar, the uniform and "
					            "these counters) can land WITHOUT the traversal half in "
					            "VoxelBrickTraverse.ush, and in that state the arm is inert BY "
					            "CONSTRUCTION and this warning is correct rather than a bug -- "
					            "grep MarchZCutEnable in the .ush before looking anywhere "
					            "else."),
					       (unsigned long long)HW.Frames);
				}
				else if (HW.bZCutArmed && HW.ZCutSkipped == 0 && HW.ZCutClipped == 0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    voxel.March.ZCut is 1, the bound WAS consulted %llu times "
					            "and removed nothing at all. That is a real measured null: "
					            "the slabs are wider than the segments. Check the per-slot "
					            "spans printed above before blaming the traversal."),
					       (unsigned long long)HW.ZCutConsulted);
				}
			}
		}
		// ---- THE TERRAIN HEIGHT PYRAMID: ARMED, FILLED, AND WHAT IT SKIPPED
		//
		// THREE QUESTIONS, PRINTED SEPARATELY, BECAUSE THEY FAIL SEPARATELY.
		//
		//   1. armed    the cvar. This arm is a uniform, so the cvar is the arm.
		//   2. FILLED   what the FIELD actually holds. A field that is entirely
		//               +INF is inert by construction -- every air test false,
		//               every counter a structural zero -- and without this line
		//               that reads as a clean null result. This is the question
		//               the Z bound does not have and this one does, because the
		//               field is BUILT over seconds rather than being a uniform.
		//   3. used     what the GPU did with it. Needs voxel.March.HoleStats.
		//               consulted == 0 while armed is ARMED AND INERT.
		//
		// and the falsifier, which is printed whenever the arm is on and is NOT
		// reported as a pass unless it was actually run.
		{
			const FVoxelMarchHoleStats HP = VoxelMarchPeekLastHoleWindow();
			const int32 RanEnable = GVoxelMarchHeightRanEnable.load(std::memory_order_relaxed);
			const int32 RanDim = GVoxelMarchHeightRanDim.load(std::memory_order_relaxed);
			const int32 RanLeaf = GVoxelMarchHeightRanLeafLevel.load(std::memory_order_relaxed);
			const int32 RanMips = GVoxelMarchHeightRanMips.load(std::memory_order_relaxed);
			// LEAF EDGE FROM THE LEVEL, not from a remembered metre value: a
			// level-L chunk is 3.2 * 2^L m and that relation is the only place
			// the two spellings can be made to agree.
			const double LeafEdgeM = (RanLeaf >= 0) ? 3.2 * double(int64(1) << RanLeaf) : 0.0;
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("  heightPyramid: armed=%d bind=%s leaf=L%d (%.1f m) dim=%d mips=%d"),
			       HP.bHeightArmed ? 1 : 0,
			       RanEnable < 0 ? TEXT("NEVER BOUND")
			                     : (RanEnable != 0 ? TEXT("enabled") : TEXT("bound, disabled")),
			       RanLeaf, LeafEdgeM, RanDim, RanMips);
			if (HP.HeightDim > 0)
			{
				const int32 LeafTotal = HP.HeightDim * HP.HeightDim;
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    field: %d/%d leaves filled (%.1f%%), %d still +INF (%.1f%%) "
				            "-- %d declined, %d fine tile not resident, %d raised by edits; "
				            "finite range %.1f .. %.1f m"),
				       HP.HeightFilledLeaves, LeafTotal,
				       LeafTotal > 0 ? 100.0 * double(HP.HeightFilledLeaves) / double(LeafTotal) : 0.0,
				       HP.HeightInfiniteLeaves,
				       LeafTotal > 0 ? 100.0 * double(HP.HeightInfiniteLeaves) / double(LeafTotal) : 0.0,
				       HP.HeightDeclinedLeaves, HP.HeightNotResidentLeaves, HP.HeightEditedLeaves,
				       HP.HeightMinUU * 0.01f, HP.HeightMaxUU * 0.01f);
				if (HP.HeightInfiniteLeaves == LeafTotal)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    THE FIELD IS ENTIRELY +INF. That is INERT, not "
					            "safe-and-working: every air test is false, the walk is "
					            "byte-for-byte the control, and every counter below is a "
					            "STRUCTURAL zero. No frame time on this leg describes the "
					            "height pyramid. Check that the builder ticked at all "
					            "(voxel.HeightPyramid.Build) before reading anything else."));
				}
			}
			if (!HP.bArmed || HP.Frames == 0)
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    engagement: NOT MEASURED (%s). Only these counters prove the "
				            "shader consulted the field; the uniform being filled does not."),
				       !HP.bArmed ? TEXT("voxel.March.HoleStats is 0")
				                  : TEXT("armed, no readback has landed yet"));
			}
			else
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    engagement: heightConsulted=%llu, advanced %llu (%.2f%%), "
				            "fully empty %llu (%.2f%%), reentries %llu; heightLeafCells=%llu, "
				            "heightSteps=%llu, over %llu frames"),
				       (unsigned long long)HP.HeightConsulted,
				       (unsigned long long)HP.HeightAdvanced,
				       HP.HeightConsulted > 0
				           ? 100.0 * double(HP.HeightAdvanced) / double(HP.HeightConsulted) : 0.0,
				       (unsigned long long)HP.HeightEmpty,
				       HP.HeightConsulted > 0
				           ? 100.0 * double(HP.HeightEmpty) / double(HP.HeightConsulted) : 0.0,
				       (unsigned long long)HP.HeightReentries,
				       (unsigned long long)HP.HeightLeafCells,
				       (unsigned long long)HP.HeightSteps,
				       (unsigned long long)HP.Frames);
				// SAID ON THE LINE ITSELF, the same sentence the other two
				// engagement groups carry and for the same three published
				// measurements: iteration counts and wall time move in OPPOSITE
				// directions often enough that a reader converting a skip
				// percentage into an expected millisecond saving is doing
				// something already known to be wrong.
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("      (engagement only -- DECISIONS, not nanoseconds. "
				            "heightLeafCells is the SKIPPED RAY LENGTH expressed in leaf "
				            "cells, so it is a LOWER bound on cells avoided. heightSteps is "
				            "the COST side and must be read next to it: a leg where both are "
				            "enormous and VoxelMarch.March does not move is a REAL NULL "
				            "RESULT, not a broken counter. The saving is VoxelMarch.March "
				            "from ProfileGPU and nothing else.)"));
				if (HP.bHeightArmed && HP.HeightConsulted == 0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    voxel.March.HeightPyramid is 1 and the shader consulted "
					            "the field ZERO times over %llu measured frames. THE ARM IS "
					            "ARMED AND INERT -- the uniform never reached this "
					            "permutation, the field was never built, or the walk never "
					            "asked. No frame time measured on this leg describes the "
					            "height pyramid."),
					       (unsigned long long)HP.Frames);
				}
				else if (HP.bHeightArmed && HP.HeightAdvanced == 0 && HP.HeightEmpty == 0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    voxel.March.HeightPyramid is 1, the field WAS consulted "
					            "%llu times and removed nothing at all. That is a real "
					            "measured null: every ray had a candidate in its first cell. "
					            "Read the field line above -- an all-+INF field produces "
					            "exactly this -- before blaming the traversal."),
					       (unsigned long long)HP.HeightConsulted);
				}
				else if (HP.bHeightArmed && HP.HeightReentries == 0 && HP.HeightAdvanced > 0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    heightReentries is 0 while heightAdvanced is %llu. The "
					            "arm is doing tStart ONLY -- the mid-ray skips the horizon "
					            "case depends on were never exercised, so this leg is a "
					            "PARTIAL result and must be written up as one. Check "
					            "voxel.March.HeightPyramid.MaxIters (1 disables them)."),
					       (unsigned long long)HP.HeightAdvanced);
				}
			}
			// ---- THE FALSIFIER ------------------------------------------
			//
			// PRINTED WHENEVER THE ARM IS ON, INCLUDING WHEN THE VERIFY IS OFF,
			// because "not tested" and "tested and clean" are different findings
			// and a suppressed line makes them identical. A zero from an unarmed
			// verify is not a pass; it is the absence of a test, and this refuses
			// to word it as anything else.
			if (HP.bHeightArmed)
			{
				if (!HP.bHeightVerifyArmed)
				{
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("    falsifier: NOT RUN (voxel.March.HeightPyramid.Verify is "
					            "0). heightMissed=%llu heightLate=%llu here is the absence of "
					            "a test, NOT a pass -- an unarmed falsifier cannot produce "
					            "either."),
					       (unsigned long long)HP.HeightMissed,
					       (unsigned long long)HP.HeightLate);
				}
				else if (HP.HeightMissed == 0 && HP.HeightLate == 0)
				{
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("    falsifier: 0 missed, 0 late over %llu consulted rays. "
					            "THIS IS ONLY A PASS IF THE TEST CAN FAIL -- run the same "
					            "leg with voxel.March.HeightPyramid.BiasM 50 and confirm "
					            "BOTH go non-zero before quoting it."),
					       (unsigned long long)HP.HeightConsulted);
				}
				else
				{
					// MISSED IS PRINTED FIRST, ALONE, AND AS AN ERROR. It is the
					// only one of the two that decides anything by itself: the
					// clamped walk found NOTHING where the control found
					// geometry, which is proven air over real ground.
					if (HP.HeightMissed > 0)
					{
						UE_LOG(LogVoxelMarch, Error,
						       TEXT("    falsifier: %llu MISSED over %llu consulted rays -- "
						            "the clamped walk found NOTHING where the full walk "
						            "found geometry. With BiasM 0 that is a HOLE and the "
						            "arm must go off. With BiasM non-zero it is the "
						            "deliberate corruption firing, which is what proves the "
						            "test can fail."),
						       (unsigned long long)HP.HeightMissed,
						       (unsigned long long)HP.HeightConsulted);
					}
					else
					{
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("    falsifier: 0 MISSED over %llu consulted rays -- no "
						            "ray lost its geometry outright."),
						       (unsigned long long)HP.HeightConsulted);
					}
					// LATE, WITH ITS DISTRIBUTION, because the count alone
					// cannot separate ring substitution from a hole. THE SHAPE
					// IS THE VERDICT: weight piled in B0 just past the 10 UU
					// tolerance is float and substitution noise; weight out in
					// B2..B4 is real geometry being skipped.
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("    falsifier: %llu LATE (hit further than the control by "
					            ">0.1 m), max %.2f m; distribution 0.1-1m=%llu 1-10m=%llu "
					            "10-100m=%llu 100-1000m=%llu >1000m=%llu"),
					       (unsigned long long)HP.HeightLate,
					       double(HP.HeightLateMaxUU) * 0.01,
					       (unsigned long long)HP.HeightLateBucket[0],
					       (unsigned long long)HP.HeightLateBucket[1],
					       (unsigned long long)HP.HeightLateBucket[2],
					       (unsigned long long)HP.HeightLateBucket[3],
					       (unsigned long long)HP.HeightLateBucket[4]);
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("      (LATE IS NOT AUTOMATICALLY A HOLE. Restarting the "
					            "walk mid-ray can put a segment in a coarser ring, and a "
					            "coarser level answering it hits at a slightly different t "
					            "-- that is the `substituted` mechanism and it is visible in "
					            "the substituted rate, which must be read beside this. A "
					            "tight pile in 0.1-1m is that. A long tail is geometry the "
					            "pyramid skipped, and that IS a hole.)"));
				}
			}
		}
		// ---- THE COARSE OCCUPANCY LEVEL: ARMED, AND WHAT IT SKIPPED -------
		//
		// TWO QUESTIONS, PRINTED SEPARATELY, BECAUSE THEY FAIL SEPARATELY AND
		// THERE IS NO "uploaded" MIDDLE TERM HERE. The Z bound is a uniform, so
		// it has three states (asked / uploaded / used). This is a PERMUTATION:
		// the code is either compiled into the kernel or it is not, so the only
		// questions are "was the arm selected" and "did the shader use it".
		//
		//   1. armed   the arm actually selected for the dispatch, NOT the raw
		//              cvar -- rings off forces it down and the permutation for
		//              that pairing does not exist.
		//   2. used    what the GPU did with it. Needs voxel.March.HoleStats.
		//              consulted == 0 while armed is ARMED AND INERT, which is
		//              this project's house failure and is printed as a warning
		//              rather than left to be inferred from a silence.
		//
		//   plus fallback, which is neither: frames that marched against an
		//   ALL-ONES coarse level because no block buffer existed yet. Those
		//   frames behave exactly like the control, so a leg with a climbing
		//   fallback count has not measured the arm at all.
		{
			const FVoxelMarchHoleStats BW = VoxelMarchPeekLastHoleWindow();
			const FVoxelMarchChunkIndex& BlkIdx = GetGlobalVoxelMarchChunkIndex();
			const uint64 Fallback = BlkIdx.GetBlockFallbackBinds();
			const uint64 BindCalls = BlkIdx.GetBlockBindCalls();
			// PRINTED AS A RATIO AGAINST BINDS, AND THAT IS A CORRECTION.
			// This line used to print the bare cumulative fallback count beside
			// the per-window frame count on the line below it, and the two were
			// read as a rate: "210 over 349 frames" was taken to mean 60% of
			// frames, when VoxelMarchBindPool runs up to FOUR times a frame and
			// 210 was ~15% of about 1,400 binds. The denominator now travels
			// with the numerator so the ratio cannot be assembled out of two
			// numbers that do not share one.
			//
			// BOTH ARE CUMULATIVE SINCE BOOT and the word says so, because the
			// engagement line beneath is per-window and the mixture is exactly
			// what misled once already.
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("  blockSkip: armed=%d fallbackBinds=%llu of %llu binds (%.2f%%, "
			            "cumulative since boot)"),
			       BW.bBlockSkipArmed ? 1 : 0, (unsigned long long)Fallback,
			       (unsigned long long)BindCalls,
			       BindCalls > 0 ? 100.0 * double(Fallback) / double(BindCalls) : 0.0);
			// WHAT A CLEAN LEG LOOKS LIKE, said here so the next reader does not
			// have to decide what "small" means: a SMALL CONSTANT from genuine
			// startup -- the binds between the index's first upload and that
			// graph's extraction landing -- and then FLAT. A count that keeps
			// climbing during a static leg is the lifetime defect of
			// 2026-08-25, and FVoxelMarchChunkIndex logs it by name past its
			// grace count.
			if (BW.bBlockSkipArmed && Fallback > 0)
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("      (fallback binds marched against an ALL-ONES coarse level, "
				            "i.e. against the CONTROL -- they descend into every block and "
				            "cannot skip. Expect a small constant from startup, then FLAT. "
				            "Still climbing = the timing on this leg is contaminated.)"));
			}
			if (!BW.bArmed || BW.Frames == 0)
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    engagement: NOT MEASURED (%s). Only these counters prove the "
				            "shader consulted the coarse level; the permutation being selected "
				            "does not."),
				       !BW.bArmed ? TEXT("voxel.March.HoleStats is 0")
				                  : TEXT("armed, no readback has landed yet"));
			}
			else
			{
				// consulted - skipped IS the count of blocks that were read,
				// found empty or occupied, and stepped through anyway -- so the
				// two partition every decision with no residue.
				const uint64 BlkNotSkipped = BW.BlockConsulted >= BW.BlockSkipped
				                                 ? BW.BlockConsulted - BW.BlockSkipped
				                                 : 0;
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    engagement: blkConsulted=%llu = blkSkipped %llu (%.2f%%) + "
				            "stepped %llu; blkCellsAvoided=%llu, over %llu frames"),
				       (unsigned long long)BW.BlockConsulted,
				       (unsigned long long)BW.BlockSkipped,
				       BW.BlockConsulted > 0
				           ? 100.0 * double(BW.BlockSkipped) / double(BW.BlockConsulted)
				           : 0.0,
				       (unsigned long long)BlkNotSkipped,
				       (unsigned long long)BW.BlockCellsAvoided,
				       (unsigned long long)BW.Frames);
				// SAID ON THE LINE ITSELF, not only in a header nobody reading a
				// log will open -- the same sentence the Z bound's line carries,
				// and for the same three published measurements.
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("      (engagement only -- these are DECISIONS, not nanoseconds. "
				            "blkCellsAvoided is the JUMPED RAY LENGTH expressed in chunk "
				            "cells, so it is a LOWER bound on cells avoided -- a diagonal DDA "
				            "visits up to sqrt(3) times the axis count over the same span. It "
				            "is deliberately NOT 64 per skipped block, which would carry no "
				            "information beyond blkSkipped and would wrap a uint32. The saving "
				            "is VoxelMarch.March from ProfileGPU and nothing else.)"));
				// ---- THE SKY LICENCE, AND THE NUMBER IT IS JUDGED ON ------
				//
				// PRINTED WHENEVER THE BLOCK LEVEL IS ARMED, mode 0 included,
				// because "the licence is off" and "the licence is on and found
				// nothing" are different findings and a suppressed line makes
				// them identical. Mode 0 must read licensed=0: it issues no sky
				// load at all, so a non-zero there would indict the instrument.
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    sky licence: mode=%d, blkSkyLicensed=%llu of %llu consulted "
				            "(%.2f%%), run mean %.2f blocks (%llu blocks / %llu runs)"),
				       BW.BlockSkyMode,
				       (unsigned long long)BW.BlockSkyLicensed,
				       (unsigned long long)BW.BlockConsulted,
				       BW.BlockConsulted > 0
				           ? 100.0 * double(BW.BlockSkyLicensed) / double(BW.BlockConsulted)
				           : 0.0,
				       BW.BlockSkyRuns > 0
				           ? double(BW.BlockSkyRunBlocks) / double(BW.BlockSkyRuns)
				           : 0.0,
				       (unsigned long long)BW.BlockSkyRunBlocks,
				       (unsigned long long)BW.BlockSkyRuns);
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("      (THE RUN MEAN IS THE DECIDING NUMBER, and it is a count of "
				            "DECISIONS. One licensed block is a 4-chunk advance off one load, "
				            "so break-even is a run of 1.0 and everything above it is margin. "
				            "A mean near 1.0 against a HIGH licensed rate says open sky is "
				            "SCATTERED at 4^3 granularity -- that indicts the BLOCK SIZE, not "
				            "the mark, and the next move would be a coarser block rather than "
				            "a different predicate. The saving is still VoxelMarch.March from "
				            "ProfileGPU and nothing else.)"));
				if (BW.BlockSkyMode != 0 && BW.BlockSkyLicensed == 0 && BW.BlockConsulted > 0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    voxel.March.BlockSkySkip is %d and the marcher found ZERO "
					            "sky-licensed blocks over %llu consults. THE LICENCE IS ARMED "
					            "AND INERT, and no frame time on this leg describes it. The "
					            "cause is almost certainly upstream, not here: this arm reads "
					            "a bit the STREAMING side writes. Check, in order -- (1) is "
					            "voxel.Stream.SkyMark 1 (it defaults to 0); (2) the 'skyMark "
					            "CELLS' line on the LogVoxelPerf 5 s log, where written=0 of "
					            "offered>0 names the cause and a DISCONNECTED SINK warning "
					            "names it exactly; (3) the 'skyMark COLUMNS' line, where "
					            "marked=0 of considered>0 means no column passed the coverage "
					            "proof. A block needs ALL 64 of its cells marked, so a partial "
					            "band licenses nothing -- voxel.Stream.SkyMarkChunks below 4 "
					            "cannot license a single block at any relief."),
					       BW.BlockSkyMode, (unsigned long long)BW.BlockConsulted);
				}
				if (BW.bBlockSkipArmed && BW.BlockConsulted == 0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    voxel.March.BlockSkip is armed and the shader consulted "
					            "the coarse level ZERO times over %llu measured frames. THE "
					            "ARM IS ARMED AND INERT -- no frame time measured on this leg "
					            "describes the coarse level. FIRST THING TO CHECK: the C++ "
					            "half of this change (the cvar, the permutation, the buffers "
					            "and these counters) can land WITHOUT the traversal half in "
					            "VoxelBrickTraverse.ush, and in that state the arm is inert BY "
					            "CONSTRUCTION and this warning is correct rather than a bug -- "
					            "grep VOXEL_MARCH_BLOCK_SKIP in the .ush before looking "
					            "anywhere else. SECOND: fallbackBinds above."),
					       (unsigned long long)BW.Frames);
				}
				else if (BW.bBlockSkipArmed && BW.BlockSkipped == 0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    voxel.March.BlockSkip is armed, the coarse level WAS "
					            "consulted %llu times and skipped nothing at all. That is a "
					            "real measured null, not a broken counter: either every block "
					            "the rays crossed held a resident chunk, or the block exit "
					            "never beat the ordinary one-chunk advance. Check "
					            "fallbackBinds above before blaming the traversal."),
					       (unsigned long long)BW.BlockConsulted);
				}
			}
		}
		// ---- THE MOVING LATTICE: ASKED, RAN, AND WHETHER IT IS MOVING ------
		//
		// THREE FIELDS, AND THE THIRD IS THE ONE THAT MATTERS. 'asked' and 'ran'
		// are the same asked/ran discipline the halfRes line above uses, and
		// they catch a switch that never reached a dispatch. They do NOT catch
		// the failure this arm is actually exposed to, which is a lattice that
		// is armed, dispatched, and never moves -- a pinned phase, a cycle
		// length of one, a frame counter that does not advance on a paused
		// leg. So 'phasesSeen' is a popcount of the phases the march has really
		// dispatched since the scheme last changed, and it is the falsifier:
		//
		//     phasesSeen = cycle   the lattice is moving; the arm is engaged
		//     phasesSeen = 1       the lattice is STATIC, whatever the cvar
		//                          says, and every image taken since describes
		//                          the arm this was built to replace
		//
		// This is the same reading the raster atlas's 'fill: mode=N' line exists
		// to give, applied to a thing that changes over TIME rather than over
		// configuration -- which is why a single-frame line could not have said
		// it and a running mask can.
		{
			const int32 RanScheme = GVoxelMarchRanJitterScheme.load(std::memory_order_relaxed);
			const int32 RanPhase = GVoxelMarchRanJitterPhase.load(std::memory_order_relaxed);
			const int32 RanCycle = GVoxelMarchRanJitterCycle.load(std::memory_order_relaxed);
			const uint32 Mask = GVoxelMarchRanJitterPhaseMask.load(std::memory_order_relaxed);
			const uint64 JitterFrames = GVoxelMarchRanJitterFrames.load(std::memory_order_relaxed);
			int32 PhasesSeen = 0;
			for (uint32 M = Mask; M != 0u; M >>= 1)
			{
				PhasesSeen += int32(M & 1u);
			}
			const int32 AskedScheme =
				FMath::Clamp(CVarVoxelMarchHalfResJitter.GetValueOnAnyThread(), 0, 2);
			const int32 AskedPhase = CVarVoxelMarchHalfResJitterPhase.GetValueOnAnyThread();
			// Recomputed from the SAME table the dispatch used, so this line
			// cannot report an offset the shader never carried.
			const FVector2f RanOffset =
				(RanScheme > 0) ? VoxelMarchJitterOffset(RanScheme, RanPhase)
				                : FVector2f::ZeroVector;
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("  halfResJitter: asked=%d ran=%s | phase=%s of %d | last offset="
			            "(%+.4f, %+.4f) full-res px | phasesSeen=%d/%d mask=0x%02X | "
			            "pin=%s | frames=%llu"),
			       AskedScheme,
			       RanScheme < 0 ? TEXT("never-ran")
			                     : (RanScheme == 0 ? TEXT("0 (static block centre)")
			                                       : (RanScheme == 1 ? TEXT("1 (4-frame quad)")
			                                                         : TEXT("2 (8-frame Halton)"))),
			       RanPhase < 0 ? TEXT("never-ran") : *FString::FromInt(RanPhase),
			       RanCycle, RanOffset.X, RanOffset.Y, PhasesSeen, FMath::Max(RanCycle, 1), Mask,
			       AskedPhase < 0 ? TEXT("free-running")
			                      : *FString::Printf(TEXT("PINNED to %d"), AskedPhase),
			       JitterFrames);
			if (RanScheme > 0 && PhasesSeen <= 1 && JitterFrames > 4)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("  voxel.March.HalfRes.Jitter is %d but the march has dispatched "
				            "exactly ONE lattice position across %llu frames (mask 0x%02X). The "
				            "sample lattice is STATIC, so TSR is being handed the same %d%% of "
				            "pixels every frame and has nothing new to accumulate -- which is "
				            "precisely the condition this arm exists to remove. Check "
				            "voxel.March.HalfRes.JitterPhase (a pinned phase IS a static "
				            "lattice) before reading any image from this leg as evidence about "
				            "temporal reconstruction."),
				       RanScheme, JitterFrames, Mask, 25);
			}
			// THE FULL-RES CASE IS NOT A DEFECT AND MUST NOT PRINT AS ONE.
			// The resolve forces the scheme to 0 whenever the march ran full
			// res, so asked != ran is EXPECTED there and is reported as a note.
			// Only a half-res leg whose scheme did not take is a warning.
			const int32 RanShiftForJitter = GVoxelMarchRanResShift.load(std::memory_order_relaxed);
			if (RanShiftForJitter == 0 && AskedScheme != 0)
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("  voxel.March.HalfRes.Jitter %d is IGNORED because the march ran "
				            "FULL RES. There is no 2x2 block to move a sample inside, and a "
				            "jittered full-res march would hand the emit a t measured along a "
				            "different ray for every pixel on screen. Set voxel.March.HalfRes 1 "
				            "first."),
				       AskedScheme);
			}
			else if (RanScheme >= 0 && RanScheme != AskedScheme)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("  voxel.March.HalfRes.Jitter asks for %d and the march last RAN %d. "
				            "One frame of this is a live cvar change settling. Anything longer "
				            "means the switch is armed and not engaging, and every image taken "
				            "since describes the other lattice."),
				       AskedScheme, RanScheme);
			}
		}
		if (Arm.Source == 1 && S.IndexEntries == 0)
		{
			UE_LOG(LogVoxelMarch, Warning,
			       TEXT("  source=brickpool but the chunk index holds ZERO entries. The pool and "
			            "the marcher have not met -- every ray will miss and the frame will look "
			            "like an empty world rather than like a failure. Check that the pool has "
			            "flushed (voxel.Brick.Stats) before reading anything else here."));
		}
		// THE GUARD GAP, CLOSED. The check above catches an EMPTY index. It does
		// not catch a POPULATED index whose rays still mostly miss -- which is
		// the failure that actually happened, and it printed a healthy-looking
		// line. A populated index with almost no hit tiles should be as loud as
		// an empty one, so it is.
		//
		// The 15% floor is not a quality bar. It is the observed occupancy-source
		// hit rate at the working pose divided by four: below that, the two
		// sources disagree about the world by more than any legitimate predicate
		// difference could explain, given that m != MAT_AIR is a SUPERSET of the
		// fluid predicate and the brick volume must therefore mark MORE solid.
		else if (Arm.Source == 1 && S.TilesTotal > 0 &&
		         S.TilesHit * 100u < S.TilesTotal * 15u)
		{
			UE_LOG(LogVoxelMarch, Warning,
			       TEXT("  source=brickpool drew only %u of %u tiles (%.1f%%) with a POPULATED "
			            "index (%d entries). That is not a cost result, it is a symptom: a miss "
			            "runs to the full step budget and a hit terminates early, so few hits AND "
			            "high cost means rays are failing to find terrain that is there. Do NOT "
			            "read marchMs as the indirection cost. Run voxel.March.VerifyIndex 1."),
			       S.TilesHit, S.TilesTotal,
			       100.0 * double(S.TilesHit) / double(S.TilesTotal), S.IndexEntries);
		}

		// The source comparator.
		const FVoxelMarchStats::FSourceCompare& S_ = S.SourceCompare;
		// TWO LEGS THAT AGREE TO THE DIGIT ARE NOT TWO MEASUREMENTS. A pair of
		// legs came back byte-identical on every arbiter figure across a
		// population that has otherwise swung 695x -- which is either the same
		// world sampled twice (so N is one smaller than it looks) or a readback
		// that never refreshed. Both are silent, and this project has been
		// bitten by silent staleness repeatedly, so the report says so itself
		// rather than leaving it to be noticed.
		{
			static uint64 LastHash = 0;
			static uint32 LastTotal = 0;
			static uint32 LastFlatOnly = 0;
			static uint32 LastHierOnly = 0;
			static bool bHaveLast = false;
			if (bHaveLast && S_.ArbTotal == LastTotal &&
			    S_.ArbFlatOnly == LastFlatOnly && S_.ArbHierOnly == LastHierOnly)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("  IDENTICAL TO THE PREVIOUS REPORT (arbTotal=%u flatOnly=%u "
				            "hierOnly=%u). indexHash now 0x%016llx, previously 0x%016llx -- "
				            "EQUAL hashes mean the same world was sampled twice and this is "
				            "not an independent leg; DIFFERENT hashes mean the counters did "
				            "not refresh and the readback is stale. Do not count this as N+1 "
				            "either way."),
				       S_.ArbTotal, S_.ArbFlatOnly, S_.ArbHierOnly, S_.IndexHash, LastHash);
			}
			LastHash = S_.IndexHash;
			LastTotal = S_.ArbTotal;
			LastFlatOnly = S_.ArbFlatOnly;
			LastHierOnly = S_.ArbHierOnly;
			bHaveLast = true;
		}
		const FVoxelMarchStats::FSourceCompare& C = S_;
		if (C.bValid)
		{
			const uint32 Disagree = C.BrickMissed + C.BrickExtra + C.Hard;
			if (!C.bOccValid)
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("Voxel march source compare: NOT RUN. The march frame is anchored on "
				            "the camera (deterministic) rather than on the fluid volume, and the "
				            "occupancy walk resolves world voxels through the volume's own "
				            "origin -- so it cannot be asked about these rays. This is not a "
				            "pass and not a failure. Set voxel.March.Source 0 for that "
				            "comparison; it is settled over seven clean runs."));
			}
			else
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("Voxel march source compare: rays=%u | agree miss=%u hit=%u | "
				            "ties=%u | DISAGREE brickMissed=%u brickExtra=%u hard=%u | excluded "
				            "watermark=%u | control occNoise=%u"),
				       C.Rays, C.AgreeMiss, C.AgreeHit, C.Ties, C.BrickMissed, C.BrickExtra,
				       C.Hard, C.Watermark, C.OccNoise);
			}
			// AGAINST THE CONTROL, NOT AGAINST ZERO. Phase 0's rule, and its
			// asymmetric verdict: above the floor is a hard result, below it is
			// consistent with float ordering noise and is not proof of anything.
			if (Disagree > C.RefNoise)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("  %u disagreements against a reference noise floor of %u -- ABOVE "
				            "THE FLOOR, so this is a hard result and not sampling noise."),
				       Disagree, C.RefNoise);
			}
			else
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("  %u disagreements against a reference noise floor of %u -- within "
				            "the floor. Consistent with float ordering noise; NOT proof the two "
				            "walks agree."),
				       Disagree, C.RefNoise);
			}
			// ---- THE SKIP RATIO, and the gate on it ------------------------
			if (S_.StepsFlat > 0u)
			{
				const double Ratio = double(S_.StepsFlat) / double(FMath::Max(S_.StepsHier, 1u));
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("  SKIP RATIO: stepsFlat=%u stepsHier=%u -> %.2fx | occupancy "
				            "stepsOcc=%u | hierVsFlat disagree=%u (control refNoise=%u)"),
				       S_.StepsFlat, S_.StepsHier, Ratio, S_.StepsOcc, S_.HierVsFlat, S_.RefNoise);
				// THE CONDITIONS THE SAMPLE WAS TAKEN UNDER, from the same
				// readback. Two runs whose residency differs are two runs over
				// two worlds, and their per-term counts are not comparable
				// however identical the rays were.
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    sampled at: indexResidency=%u of poolLevel0=%u | indexHash="
				            "0x%016llx | uploads=%u, quiet for %d frames"),
				       S_.Residency, S_.PoolLevel0, S_.IndexHash, S_.Uploads, S.IndexQuietFrames);
				// THE COMPARISON RULE, printed with the numbers. Equal counts are
				// not equal contents; equal HASHES are.
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    compare per-term counts across runs ONLY at equal indexHash. "
				            "Equal residency is NOT sufficient -- 26,217 vs 26,211 came with a "
				            "150x swing in `lost`."));
				if (S.StreamingConvergedFrames < 0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    STREAMING CONVERGENCE WAS NOT CHECKED for this sample: "
					            "VoxelMarchPublishStreamingState is never called. The gate ran on "
					            "index completeness alone, which is measured against a pool that "
					            "itself varies between legs. Treat a hash mismatch as expected."));
				}
				if (S_.PoolLevel0 > 0u && S_.Residency != S_.PoolLevel0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    INDEX IS INCOMPLETE: %u of %u level-0 chunks. The volume was "
					            "not converged when this was sampled -- an upload-quiet window "
					            "during streaming is a LULL, not convergence, and this project "
					            "has already had a leg read one as settled."),
					       S_.Residency, S_.PoolLevel0);
				}
				// HIT/MISS FOR THE HIERARCHY PATH, OUTSIDE THE DISAGREE BLOCK.
				// agreeMiss/agreeHit belong to the source compare, which is dark on
				// every camera-anchored frame, so this is the only hit/miss the
				// ratio can cite -- and a leg with ZERO disagreements is exactly a
				// leg whose ratio gets quoted, so it must print there too.
				// It qualifies the ratio: the hierarchy wins most on rays crossing
				// empty space, so a mostly-empty scene inflates it.
				{
					const double HitPct = (S_.Rays > 0u)
						? 100.0 * double(S_.FlatHit) / double(S_.Rays) : 0.0;
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("    scene fill: flat hit=%u of %u rays (%.1f%%) | hier "
					            "hit=%u | RATIO SCOPE: a low hit %% means the ratio is "
					            "measured mostly on empty-space rays, where the hierarchy "
					            "wins most and the number is correspondingly optimistic"),
					       S_.FlatHit, S_.Rays, HitPct, S_.HierHit);
					// ---- THE MODE FINGERPRINT -------------------------------
				// ONE GREPPABLE LINE identifying the world this leg marched, so
				// legs can be bucketed by hash instead of by eye. Per-term counts
				// are comparable across legs ONLY at equal hash -- nine rounds of
				// B-2a were spent reading round-to-round deltas of a bimodal
				// quantity as if it were one.
				//
				// NO "RATE PER MILLION COMPARED" HERE, and the plan asked for one.
				// That was written before the denominators were measured: rays
				// compared barely moves between legs, so dividing by it is
				// arithmetically inert and reproduces the raw spread while
				// looking rigorous. Levels and hit populations go on the line
				// instead, because those DO vary and they say what the leg saw.
				{
					const FVoxelMarchChunkIndex& Idx = GetGlobalVoxelMarchChunkIndex();
					// ---- G1: ONE GREPPABLE LINE PER PRINT --------------------
					// `grep MARCHFP` gives one row per print. Two prints of one
					// leg are then diffable at a glance, which is the whole test
					// of whether they described the same world.
					//
					// SEQ makes two prints orderable, and every field on this row
					// is stamped AT DISPATCH -- frame and quiet included -- so it
					// describes the frame that produced the counts rather than
					// the moment the line was written.
					{
						static uint32 GPrintSeq = 0;
						++GPrintSeq;
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("MARCHFP seq=%u frame=%u quiet=%u hash=0x%016llx rays=%u "
						            "disagree=%u flatOnly=%u hierOnly=%u ratio=%.2f"),
						       GPrintSeq, S_.FrameNumber, S_.QuietFrames, S_.IndexHash,
						       S_.Rays, S_.HvfLost + S_.HvfGained + S_.HvfMoved,
						       S_.ArbFlatOnly, S_.ArbHierOnly,
						       (S_.StepsHier > 0u)
						           ? double(S_.StepsFlat) / double(S_.StepsHier) : 0.0);
					}
					// ---- E: THE GPU/CPU JOIN, CHECKED --------------------------
					// The stamp and the index object must agree. They are read
					// from different places and only one of them can be culled,
					// so a mismatch is the stamp failing rather than the world
					// changing -- which is precisely the confirmation the last
					// round could not perform.
					{
						const int32 CpuResidency = Idx.GetNumEntries();
						const uint64 CpuUploads = Idx.GetUploads();
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("  JOIN residency gpu=%u cpu=%d | uploads gpu=%u cpu=%llu"),
						       S_.GpuResidency, CpuResidency, S_.GpuUploads, CpuUploads);
						// THE ARM, READ FROM THE KERNEL RATHER THAN FROM THE CVAR.
						// A cvar says what was asked for; the stamp says what was
						// compiled. The comparator ran rings-off for its whole
						// existence while the CPU believed otherwise, and nothing
						// in the output could say so.
						const bool bArmRingsGpu = (S_.ArmStamp & 1u) != 0u;
						if (S_.Unattributed > 0u)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("  JOIN: LEVEL ATTRIBUTION BROKEN -- %u uncontested rays "
							            "were attributed by an arm that did not hit, so their "
							            "level field is unset and reads as a legal 0. The "
							            "per-level split is landing them all on L0 and cannot "
							            "describe any ring above it."),
							       S_.Unattributed);
						}
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("  JOIN arm rings: shader=%d cpu=%d | shader skip=%u "
						            "source=%d fallthrough=%u (stamp 0x%02x)"),
						       bArmRingsGpu ? 1 : 0, S_.bArmRingsCpu ? 1 : 0,
						       (S_.ArmStamp >> 1u) & 3u, ((S_.ArmStamp >> 3u) & 1u) ? 1 : 0,
						       (S_.ArmStamp >> 4u) & 3u, S_.ArmStamp);
						if (bArmRingsGpu != S_.bArmRingsCpu)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("  JOIN: ARM MISMATCH -- the frame was built for "
							            "rings=%d and the kernel was compiled for rings=%d. "
							            "The CPU's box, origin and reach do not match the "
							            "shader's, so EVERY figure in this report describes a "
							            "configuration that does not exist. Do not read it."),
							       S_.bArmRingsCpu ? 1 : 0, bArmRingsGpu ? 1 : 0);
						}
						if (S_.GpuResidency == 0u && CpuResidency != 0)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("  JOIN: THE CONDITION STAMP DID NOT RUN -- gpu "
							            "residency 0 against cpu %d. Everything stamped this "
							            "dispatch (hash, uploads, poolL0, frame, quiet) is a "
							            "default, not a measurement, and this print cannot be "
							            "bucketed against any other."),
							       CpuResidency);
						}
					}
					// ---- F: PER-FRAME vs CUMULATIVE ---------------------------
					// The row below mixes two provenances and used to say so
					// nowhere. Everything from the compare buffer is ONE FRAME --
					// it is cleared every dispatch -- while everything from the
					// index object is live state or monotonic since attach.
					// Counts from one print are a snapshot; ratios from one print
					// are comparable, which is measured: across six prints the
					// ratio held to 3.9% while the counts under it moved 2.3x and
					// the disagreement count 37x.
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("  MODEFP [per-frame: rays, hit*, ringDiscordant, disagree, "
					            "flat/hier-only] [cumulative: resident, uploads, alias]"));
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("  MODEFP hash=0x%016llx resident %d/%d/%d/%d/%d/%d rays=%u "
					            "hit %u/%u/%u/%u/%u/%u ringDiscordant=%u | index uploads=%llu "
					            "(%llu MiB total)"),
					       S_.IndexHash,
					       Idx.GetNumEntriesAtLevel(0), Idx.GetNumEntriesAtLevel(1),
					       Idx.GetNumEntriesAtLevel(2), Idx.GetNumEntriesAtLevel(3),
					       Idx.GetNumEntriesAtLevel(4), Idx.GetNumEntriesAtLevel(5),
					       S_.Rays,
					       S_.HitAtLevel[0], S_.HitAtLevel[1], S_.HitAtLevel[2],
					       S_.HitAtLevel[3], S_.HitAtLevel[4], S_.HitAtLevel[5],
					       S_.RingDiscordant,
					       Idx.GetUploads(),
					       Idx.GetUploadBytes() / (1024ull * 1024ull));
					// ---- B: WHERE RAYS LEFT BEFORE BEING COUNTED --------------
					{
						const uint32 Dropped = S_.EarlyOut[0] + S_.EarlyOut[1] + S_.EarlyOut[2];
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("  MODEFP early-outs: outOfBounds=%u rayMissedVolume=%u "
						            "depthClipped=%u | counted=%u of %u issued"),
						       S_.EarlyOut[0], S_.EarlyOut[1], S_.EarlyOut[2],
						       S_.Rays, S_.Rays + Dropped);
						if (S_.EarlyOut[1] + S_.EarlyOut[2] > S_.Rays)
						{
							UE_LOG(LogVoxelMarch, Warning,
							       TEXT("  MODEFP: MOST RAYS NEVER REACHED THE COMPARATOR -- "
							            "%u dropped against %u counted. Every per-ray figure "
							            "below describes the surviving minority, and a change "
							            "in this split between legs moves them all without "
							            "anything about the walk changing."),
							       S_.EarlyOut[1] + S_.EarlyOut[2], S_.Rays);
						}
					}
					// ---- WHERE THE DISAGREEMENT IS -----------------------------
					// Four legs of bursts with no surviving mechanism: no fixed
					// print index, a frozen hash, thousands of quiet frames, and
					// the SIGN FLIPS between occurrences. Every directional story
					// died on the second leg. This does not need a mechanism to
					// be useful -- it halves the hypothesis space by shape.
					{
						int32 Tiles = 0;
						for (int32 i = 0; i < 8; ++i)
						{
							Tiles += FMath::CountBits(uint64(S_.DisTileMask[i]));
						}
						const int32 BoxW =
							FMath::Max(S_.DisBoxMax.X - S_.DisBoxMin.X + 1, 0);
						const int32 BoxH =
							FMath::Max(S_.DisBoxMax.Y - S_.DisBoxMin.Y + 1, 0);
						const double FramePct =
							(S_.ViewRectSize.X > 0 && S_.ViewRectSize.Y > 0)
								? 100.0 * double(BoxW) * double(BoxH) /
									  (double(S_.ViewRectSize.X) * double(S_.ViewRectSize.Y))
								: 0.0;
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("  MODEFP disagreement shape: %d of 256 tiles occupied | "
						            "box (%d,%d)-(%d,%d) = %dx%d px, %.1f%% of frame"),
						       Tiles, S_.DisBoxMin.X, S_.DisBoxMin.Y, S_.DisBoxMax.X,
						       S_.DisBoxMax.Y, BoxW, BoxH, FramePct);
						if (Tiles == 0)
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("  MODEFP shape: no disagreeing rays this print."));
						}
						else if (Tiles <= 16)
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("  MODEFP shape: CLUSTERED -- %d tiles. The disagreement "
							            "is about a PLACE, which points at residency or an "
							            "upload event rather than at timing."),
							       Tiles);
						}
						else if (Tiles >= 128)
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("  MODEFP shape: FRAME-WIDE SCATTER -- %d of 256 tiles. "
							            "Nothing about the geometry there; this is timing, a "
							            "race, or something per-wave."),
							       Tiles);
						}
						else
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("  MODEFP shape: INTERMEDIATE -- %d of 256 tiles. "
							            "Neither clustered nor frame-wide; do not fit either "
							            "story to it."),
							       Tiles);
						}
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("  (read the TILE COUNT with the box, never either alone: "
						            "two clusters at opposite corners give the same box as a "
						            "frame-wide scatter, and only the tile count separates "
						            "them.)"));
					}
					// ---- D: ENTERED vs FOUND, PER RING ------------------------
					{
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("  MODEFP entered %u/%u/%u/%u/%u/%u | hit %u/%u/%u/%u/%u/%u "
						            "| entered a ring and hit nothing (sky) = %u"),
						       S_.EnteredLevel[0], S_.EnteredLevel[1], S_.EnteredLevel[2],
						       S_.EnteredLevel[3], S_.EnteredLevel[4], S_.EnteredLevel[5],
						       S_.HitAtLevel[0], S_.HitAtLevel[1], S_.HitAtLevel[2],
						       S_.HitAtLevel[3], S_.HitAtLevel[4], S_.HitAtLevel[5], S_.Sky);
						for (int32 L = 1; L < FVoxelMarchStats::FSourceCompare::kRingLevels; ++L)
						{
							if (S_.EnteredLevel[L] == 0u && S_.HitAtLevel[L] == 0u)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("  MODEFP: ring %d was NEVER ENTERED. Its zero says "
								            "nothing about whether it works -- rays stopped "
								            "before reaching it (occlusion, budget, or reach)."),
								       L);
							}
							else if (S_.EnteredLevel[L] > 0u && S_.HitAtLevel[L] == 0u)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("  MODEFP: ring %d was ENTERED BY %u RAYS AND HIT "
								            "NOTHING. That is a real observation about the "
								            "ring, not an absence of one."),
								       L, S_.EnteredLevel[L]);
							}
						}
					}
					// ALIASING, OBSERVED. The old span guard fired in 100% of legs
					// that attached the index -- which is what a MONOTONICALLY
					// GROWING statistic against a fixed threshold must eventually
					// do in every leg that runs long enough. It could not have
					// come out any other way, so it was not evidence. This counts
					// actual collisions instead: an add landing on a cell already
					// held by a different chunk.
					{
						int32 ATot = 0;
						for (int32 L = 0; L < int32(FVoxelMarchChunkIndex::kLevels); ++L)
						{
							ATot += Idx.GetAliasCollisions(L);
						}
						const FIntVector Sp0 = Idx.GetCumulativeCoordSpan(0);
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("  MODEFP alias collisions: %d/%d/%d/%d/%d/%d | cumulative "
						            "L0 coord span (%d,%d,%d) -- THE SPAN IS A TRAVEL LOG, NOT "
						            "AN ALIASING CLAIM: it is never reset and never shrinks "
						            "on eviction, so it grows with camera distance whether or "
						            "not any two chunks were ever resident together."),
						       Idx.GetAliasCollisions(0), Idx.GetAliasCollisions(1),
						       Idx.GetAliasCollisions(2), Idx.GetAliasCollisions(3),
						       Idx.GetAliasCollisions(4), Idx.GetAliasCollisions(5),
						       Sp0.X, Sp0.Y, Sp0.Z);
						if (ATot > 0)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("  MODEFP: %d CHUNKS WERE SHADOWED. Each is a hole the "
							            "marcher invented, and holes are exactly what the "
							            "hierarchy skips fastest -- so the skip ratio from this "
							            "leg is inflated by an unknown amount and must not be "
							            "quoted."),
							       ATot);
						}
						else
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("  MODEFP: NO CHUNK WAS SHADOWED this leg. The index "
							            "holds every chunk it was given, and the ratio is not "
							            "inflated by invented holes."));
						}
					}
					// THE OWNERSHIP SPLIT, ANSWERED RATHER THAN INFERRED. An empty
					// L1 grid has two completely different causes with two
					// different owners: the pool never offered L1 chunks
					// (residency, not this module), or the index was offered them
					// and dropped them (this module). The offered counts say which
					// without a second leg.
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("  MODEFP offered per level: L0=%d L1=%d L2=%d L3=%d L4=%d "
					            "L5=%d L6=%d | dropped above the %u indexed levels=%d (SHOULD BE 0 "
					            "at seven levels; anything else is the pool building a level "
					            "the marcher cannot walk)"),
					       Idx.GetOfferedAtLevel(0), Idx.GetOfferedAtLevel(1),
					       Idx.GetOfferedAtLevel(2), Idx.GetOfferedAtLevel(3),
					       Idx.GetOfferedAtLevel(4), Idx.GetOfferedAtLevel(5),
					       Idx.GetOfferedAtLevel(6),
					       FVoxelMarchChunkIndex::kLevels, Idx.GetDroppedWrongLevel());
					if (Idx.GetNumEntriesAtLevel(1) == 0)
					{
						if (Idx.GetOfferedAtLevel(1) == 0)
						{
							UE_LOG(LogVoxelMarch, Warning,
							       TEXT("  MODEFP: L1 GRID EMPTY AND THE POOL NEVER OFFERED "
							            "AN L1 CHUNK. Not a marcher defect -- residency is "
							            "not building level 1 at this pose. voxel.March.Rings "
							            "changes only what the MARCHER walks; it does not ask "
							            "the pool to build anything."));
						}
						else
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("  MODEFP: L1 GRID EMPTY THOUGH THE POOL OFFERED %d "
							            "L1 CHUNKS. That is this module dropping them, and it "
							            "is a marcher defect."),
							       Idx.GetOfferedAtLevel(1));
						}
					}
					// BUDGET-LIMITED OR TRAVERSAL-LIMITED. Printed on the
					// fingerprint because it decides whether ANY other number on
					// the page is a property of the walk.
					{
						const double BFlat = (S_.Rays > 0u)
							? 100.0 * double(S_.BudgetFlat) / double(S_.Rays) : 0.0;
						const double BHier = (S_.Rays > 0u)
							? 100.0 * double(S_.BudgetHier) / double(S_.Rays) : 0.0;
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("  MODEFP budget spent: flat %u (%.1f%%) hier %u (%.1f%%) "
						            "of %u rays | max flat steps %u against budget %d"),
						       S_.BudgetFlat, BFlat, S_.BudgetHier, BHier, S_.Rays,
						       S_.MaxStepsFlat,
						       CVarVoxelMarchStepBudget.GetValueOnAnyThread());
						// THE REFERENCE CLIPPED IS A DIFFERENT FAILURE FROM THE
						// HIERARCHY CLIPPED, and it is the dangerous one: a
						// truncated reference makes the hierarchy look like it is
						// FINDING content, which reads as an inverted gate and as a
						// flattering skip ratio at the same time.
						if (S_.BudgetFlat > S_.BudgetHier * 2u && S_.BudgetFlat > 0u)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("  MODEFP: THE REFERENCE IS THE ONE BEING CLIPPED -- "
							            "flat %u vs hier %u. The flat arm stops short, the "
							            "hierarchy runs on, and every ray where that happens "
							            "counts as the hierarchy finding content the reference "
							            "missed. Expect an INVERTED gate and an inflated "
							            "ratio; neither is a property of the walk."),
							       S_.BudgetFlat, S_.BudgetHier);
						}
						if (BFlat >= 5.0 || BHier >= 5.0)
						{
							UE_LOG(LogVoxelMarch, Warning,
							       TEXT("  MODEFP: BUDGET-LIMITED LEG -- %.1f%% of rays ran "
							            "out of steps rather than reaching a surface or the "
							            "segment end. Hit counts and the skip ratio are then "
							            "properties of voxel.March.StepBudget, not of the "
							            "walk, and MUST NOT be quoted as either. The budget "
							            "was sized for a 51.2 m box; this leg's reach is far "
							            "longer. DO NOT raise it to make a number look "
							            "better -- decide what the budget should be for the "
							            "reach, register it, and re-run."),
							       FMath::Max(BFlat, BHier));
						}
					}
					if (S_.HitAtLevel[1] == 0u)
					{
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("  MODEFP: NO RAY HIT IN RING 1 this leg. Nothing here "
						            "says anything about the outer ring -- not that it is "
						            "correct, only that it was not exercised."));
					}
					if (S_.RingDiscordant == 0u && S_.HitAtLevel[1] > 0u)
					{
						UE_LOG(LogVoxelMarch, Warning,
						       TEXT("  MODEFP: rays hit in BOTH rings yet ring-discordant is "
						            "ZERO. The band is +/- one coarse chunk (6.4 m at the "
						            "128 m boundary); a population that crosses the boundary "
						            "should land some pixels in it. Suspect the classifier, "
						            "not the traversal."));
					}
				}
				// ---- GATE v4, PER LEVEL ----------------------------------
				// The deliverable that justified building past a FAIL: whether
				// the uncontested asymmetry is LEVEL-DEPENDENT. The 1.5x bar is
				// a level-0 number and is NOT applied to level 1 here -- a bar
				// for coarse levels must be registered before those levels are
				// measured, not chosen once their number is visible.
				for (int32 L = 0; L < FVoxelMarchStats::FSourceCompare::kRingLevels; ++L)
				{
					const uint32 F = S_.FlatOnlyL[L];
					const uint32 Hh = S_.HierOnlyL[L];
					const uint32 Small = FMath::Min(F, Hh);
					if (F == 0u && Hh == 0u)
					{
						continue;
					}
					if (Small < 20u)
					{
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("    GATE v4 L%d: NO VERDICT -- smaller arm %u "
						            "(flat-only %u, hier-only %u). A ratio needs its "
						            "denominator to clear 20."),
						       L, Small, F, Hh);
					}
					else
					{
						const double A = double(F) / double(FMath::Max(Hh, 1u));
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("    GATE v4 L%d: asymmetry %.2fx (flat-only %u, "
						            "hier-only %u)%s"),
						       L, A, F, Hh,
						       // TWO-SIDED, MATCHING THE AGGREGATE. This read
						       // `A <= 1.5` only, so on the very numbers that made
						       // the parent line say FAIL (INVERTED) at 0.07x, this
						       // line said PASS -- a child contradicting its parent
						       // on one frame's data, because the two-sided fix
						       // landed on the aggregate and not here. A bar that
						       // cannot fail in the direction the evidence points is
						       // not a bar, and this one could not fail in the only
						       // direction the cascade has ever pointed.
						       (L == 0)
						           ? ((A > 1.5)
						                  ? TEXT(" -- FAIL: hierarchy misses real content")
						                  : ((A < (1.0 / 1.5))
						                         ? TEXT(" -- FAIL (INVERTED): the FLAT walk "
						                                "misses real content")
						                         : TEXT(" -- PASS, inside the 0.67x-1.5x band")))
						           : TEXT(" -- NO BAR REGISTERED FOR THIS LEVEL; compare it "
						                  "to L0 rather than judging it"));
					}
				}
				// THE JOIN, CHECKED RATHER THAN COMPUTED. brickEntries read
					// constant to 0.7% across six states; flatHit swings 12x across
					// windows. Those came from DIFFERENT leg sets, so it is not yet
					// a contradiction -- but rays that miss run the full reach and
					// therefore enter MORE bricks, so a 12x swing in hit fraction
					// should move bricks-per-ray substantially. If it does not,
					// one of these two is not measuring what its name says.
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("    join to check ACROSS legs: bricks/ray=%.1f at "
					            "hit=%.1f%% -- if hit%% moves 12x between legs and "
					            "bricks/ray holds within a few %%, one counter is "
					            "mislabelled and neither should qualify the ratio"),
					       (S_.Rays > 0u) ? double(S_.BrickEntries) / double(S_.Rays) : 0.0,
					       HitPct);
				}
				if (S_.HierVsFlat > 0u)
				{
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("    hierVsFlat by shape: lost=%u (skipped real content) "
					            "gained=%u (found terrain the flat walk did not) moved=%u "
					            "(both hit, different voxel)"),
					       S_.HvfLost, S_.HvfGained, S_.HvfMoved);
					// THE BIMODAL SPLIT, printed before the samples because it is
					// the shape of the defect rather than a detail of it.
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("      by brick face: lost onFace=%u offFace=%u | moved "
					            "onFace=%u offFace=%u | floor refNoise=%u"),
					       S_.LostOnFace, S_.LostOffFace, S_.MovedOnFace, S_.MovedOffFace,
					       S_.RefNoise);
					// THE COINCIDENCE VERDICT. Reported as a LIFT, never as a bare
					// share -- the bare share was the whole defect in the previous
					// instrument, which divided a per-ray verdict by an all-ray sum
					// of an unrelated quantity and could not have concluded anything.
					{
						const double DisShare = (S_.DisHit > 0u)
							? double(S_.DisThin) / double(S_.DisHit) : 0.0;
						const double BaseShare = (S_.FlatHit > 0u)
							? double(S_.HitThin) / double(S_.FlatHit) : 0.0;
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      COINCIDENCE: disagreeing rays whose hit brick "
						            "is a thin clip %u/%u (%.3f%%) vs ALL hit rays "
						            "%u/%u (%.3f%%)"),
						       S_.DisThin, S_.DisHit, DisShare * 100.0,
						       S_.HitThin, S_.FlatHit, BaseShare * 100.0);
						if (S_.DisHit == 0u || S_.FlatHit == 0u)
						{
							UE_LOG(LogVoxelMarch, Warning,
							       TEXT("      COINCIDENCE: NO VERDICT -- one side has no "
							            "population this leg."));
						}
						else if (S_.HitThin == 0u && S_.DisThin == 0u)
						{
							// The cleanest possible refutation, and it is worth
							// naming separately: the configuration does not occur
							// on hit bricks at all, so it cannot be the mechanism
							// AND no amount of further sampling will change that.
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      COINCIDENCE: MECHANISM ABSENT -- not one "
							            "hit brick on any ray is a thin clip. The corner "
							            "clip of doctrine (8) is REFUTED for this scene, "
							            "and the second brick-DDA attempt would target a "
							            "configuration that does not occur here."));
						}
						else
						{
							const double Lift = (BaseShare > 0.0)
								? DisShare / BaseShare : 0.0;
							// 3x is a deliberate bar, set BEFORE any number was
							// read, and it is about attribution rather than
							// significance: below it, a share this size is
							// reachable by disagreements being drawn from hit rays
							// at the base rate, and the test cannot separate the
							// mechanism from the population it samples.
							if (BaseShare <= 0.0)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      COINCIDENCE: MECHANISM INDICATED -- "
								            "thin clips occur on disagreeing rays and on "
								            "NO other hit ray. Lift is unbounded."));
							}
							else if (Lift >= 3.0)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      COINCIDENCE: MECHANISM INDICATED -- "
								            "lift %.2fx (bar 3.0x). Disagreements "
								            "concentrate on thin-clipped bricks. Doctrine "
								            "(8)'s diagnosis is supported and a fix must "
								            "drive disThin to zero while hitThin holds."),
								       Lift);
							}
							else
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      COINCIDENCE: MECHANISM NOT INDICATED -- "
								            "lift %.2fx (bar 3.0x). Disagreeing rays are no "
								            "more thin-clipped than hit rays generally, so "
								            "the corner clip does not select them and the "
								            "second brick-DDA attempt would not be aimed at "
								            "this defect."),
								       Lift);
							}
						}
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      (the LIFT is the result. Either share alone is "
						            "uninterpretable -- that error is what the brickEntries/"
						            "thinClips rates below were, and they are kept only as "
						            "the record of a denominator that could not co-vary.)"));
					}
					// RETAINED AND KNOWN INERT. Constants of the camera pose across
					// six states (0.7% / 0.9% spread), so these rates are the raw
					// count rescaled and nothing more. Printed so a future reader
					// meets the counter-example beside the claim it disproves.
					{
						const uint64 Dis = uint64(S_.HvfLost) + uint64(S_.HvfMoved);
						const double PerM = (S_.BrickEntries > 0)
							? double(Dis) * 1e6 / double(S_.BrickEntries) : 0.0;
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      (inert) %.2f disagree per million brick entries "
						            "(%llu entries, %llu thin clips) | raw disagree=%llu"),
						       PerM, S_.BrickEntries, S_.ThinClips, Dis);
					}
					// ---- BAND HISTOGRAM ------------------------------------
					// The magnitude of the perturbation, which the boundary
					// concentration never was. Printed as two columns because a
					// share without a base rate concluded nothing before.
					{
						FString AllRow, DisRow;
						for (int32 i = 0; i < FVoxelMarchStats::FSourceCompare::kBandBins; ++i)
						{
							AllRow += FString::Printf(TEXT("%u "), S_.BandAll[i]);
							DisRow += FString::Printf(TEXT("%u "), S_.BandDis[i]);
						}
						// Three calls, not one with escapes: a heredoc has now eaten
						// a backslash-n inside TEXT() four times in this file, and the
						// result compiles as a literal newline that only shows up in
						// the log. Removing the escape removes the trap.
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      BAND (chord through hit voxel, bin k = order "
						            "1e-k of a full crossing)"));
						UE_LOG(LogVoxelMarch, Display, TEXT("        all hits : %s"), *AllRow);
						UE_LOG(LogVoxelMarch, Display, TEXT("        disagree : %s"), *DisRow);
						// The one split this instrument makes cleanly. Bins 0-1 are
						// clean crossings; no small perturbation can flip those, so a
						// disagreement there is a logic error of order a whole voxel.
						const uint32 Coarse = S_.BandDis[0] + S_.BandDis[1];
						const uint32 DisTot = S_.DisHit;
						const double CoarsePct = (DisTot > 0u)
							? 100.0 * double(Coarse) / double(DisTot) : 0.0;
						if (DisTot == 0u)
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      BAND: no verdict -- no disagreeing hit rays."));
						}
						else if (CoarsePct >= 25.0)
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      BAND: LOGIC ERROR PRESENT -- %.1f%% of "
							            "disagreements are on rays that cut a CLEAN chord "
							            "(bins 0-1). No rounding or nudge can flip those; "
							            "something is wrong by order a whole voxel."),
							       CoarsePct);
						}
						else
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      BAND: SMALL-PERTURBATION REGIME -- only "
							            "%.1f%% of disagreements cut a clean chord. The "
							            "rest are corner grazes, consistent with drift OR "
							            "with the nudge; THIS INSTRUMENT CANNOT SEPARATE "
							            "THOSE TWO (they overlap at bins 2-4). The arbiter "
							            "is what decides it."),
							       CoarsePct);
						}
					}
					// ---- ARBITER -------------------------------------------
					{
						// CONTESTED ONLY. The uncontested outcomes are reported
						// beside it, never inside it: a `gained` ray has no flat
						// candidate and a `lost` ray has no hier candidate, so
						// neither can be evidence about which walk wins a contest.
						const uint32 Tot = S_.ArbFlat + S_.ArbHier + S_.ArbTie + S_.ArbNeither;
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      ARBITER contested (both walks offered a "
						            "candidate): flat-correct=%u hier-correct=%u tie=%u "
						            "ill-posed=%u of %u"),
						       S_.ArbFlat, S_.ArbHier, S_.ArbTie, S_.ArbNeither, Tot);
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      ARBITER uncontested: flat-only=%u (hierarchy "
						            "missed real content) hier-only=%u (the FLAT walk "
						            "missed it) -- not contests, not evidence about "
						            "which walk wins one"),
						       S_.ArbFlatOnly, S_.ArbHierOnly);
						// ---- GATE v4: the certification criterion, on the
						// uncontested population only. See VoxelMarchRenderer.h for
						// why flat-as-truth was retired and why hier-only is the
						// control rather than zero.
						{
							// THE SMALLER ARM, not the sum. A ratio's guard must test
							// its DENOMINATOR: the sum let 45/11, 27/2 and 31/19 print
							// FAIL verdicts of 4.09x, 13.50x and 1.63x on denominators
							// of 11, 2 and 19.
							const uint32 Pair = FMath::Min(S_.ArbFlatOnly, S_.ArbHierOnly);
							const uint32 ArbTot = FMath::Max(S_.ArbTotal, 1u);
							const double IllPct = 100.0 * double(S_.ArbNeither) / double(ArbTot);
							if (IllPct >= 25.0)
							{
								// The guard that was missing: ill-posed rays are drawn
								// from the uncontested population too (their only
								// candidate failed validity), so they SUPPRESS these
								// counts. A high ill-posed rate makes the gate
								// unreadable, not merely the contested verdict.
								UE_LOG(LogVoxelMarch, Warning,
								       TEXT("      GATE v4: NO VERDICT -- %.1f%% ill-posed. "
								            "Ill-posed rays are drawn from the uncontested "
								            "population as well, so flat-only=%u is a floor, "
								            "not a measurement."),
								       IllPct, S_.ArbFlatOnly);
							}
							else if (Pair < 20u)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      GATE v4: NO VERDICT -- smaller arm is only "
								            "%u (flat-only=%u hier-only=%u). A ratio needs its "
								            "DENOMINATOR to clear 20; Poisson noise on %u "
								            "exceeds the effect being measured."),
								       Pair, S_.ArbFlatOnly, S_.ArbHierOnly, Pair);
							}
							else
							{
								const double Asym = double(S_.ArbFlatOnly) /
								                    double(FMath::Max(S_.ArbHierOnly, 1u));
								// TWO-SIDED. The bar tested only flat-only <= 1.5 *
								// hier-only, so a collapse in the OTHER direction --
								// hier-only 225,022 against flat-only 6,826, an 0.03x
								// asymmetry and a NEGATIVE excess of 16% of the frame
								// -- sailed through as PASS. A ratio bounded on one
								// side is a bound, not a gate.
								//
								// Both directions are failures and they are DIFFERENT
								// failures: above the bar the hierarchy MISSES real
								// content (the skip defect); below it the hierarchy
								// finds content the flat walk does not, which is either
								// a broken reference or a hierarchy hallucinating
								// geometry. The verdict names which.
								const bool bTooHigh = Asym > 1.5;
								const bool bTooLow  = Asym < (1.0 / 1.5);
								// DETECTION AND CONSEQUENCE, PRINTED AS TWO LINES THAT
								// ARE ALLOWED TO DISAGREE. The 1.5x bar was derived as a
								// Poisson noise floor and then used as a decision
								// threshold; those are different objects (see the long
								// note in VoxelMarchRenderer.h). This line supplies the
								// detection half honestly WITHOUT being wired into the
								// verdict -- the bar is pre-registered and is not moved
								// by anything printed here.
								{
									const double LogSE = FMath::Sqrt(
										1.0 / double(FMath::Max(S_.ArbFlatOnly, 1u)) +
										1.0 / double(FMath::Max(S_.ArbHierOnly, 1u)));
									const double Sigma = (LogSE > 0.0)
										? FMath::Abs(FMath::Loge(FMath::Max(Asym, 1e-9))) / LogSE
										: 0.0;
									// The EXCESS is the hierarchy-specific population:
									// hier-only is the flat walk's own miss rate and is
									// the control, so the defect is what exceeds it.
									const int64 Excess = int64(S_.ArbFlatOnly) - int64(S_.ArbHierOnly);
									UE_LOG(LogVoxelMarch, Display,
									       TEXT("      GATE v4 detection: %.2fx is %.1f sigma "
									            "from 1.0 | hierarchy-specific excess %lld rays "
									            "(%.3f%% of %u) -- DETECTION IS COUNT-DEPENDENT "
									            "AND IS NOT THE BAR; the bar is a consequence "
									            "judgement and must not be moved by this line"),
									       Asym, Sigma, Excess,
									       (S_.Rays > 0u)
									           ? 100.0 * double(Excess) / double(S_.Rays) : 0.0,
									       S_.Rays);
								}
								if (bTooLow)
								{
									UE_LOG(LogVoxelMarch, Display,
									       TEXT("      GATE v4: FAIL (INVERTED) -- flat-only "
									            "%u vs hier-only %u, asymmetry %.2fx BELOW the "
									            "0.67x bar. The hierarchy is finding real "
									            "content the FLAT walk misses, %u rays of it. "
									            "That is not the skip defect; it is either a "
									            "broken reference or a hierarchy hallucinating "
									            "geometry, and it is a harder failure than the "
									            "high side."),
									       S_.ArbFlatOnly, S_.ArbHierOnly, Asym,
									       S_.ArbHierOnly - S_.ArbFlatOnly);
								}
								else if (!bTooHigh)
								{
									UE_LOG(LogVoxelMarch, Display,
									       TEXT("      GATE v4: PASS -- flat-only %u vs "
									            "hier-only %u, asymmetry %.2fx inside the "
									            "0.67x-1.5x band. Neither walk misses real "
									            "content more often than the other."),
									       S_.ArbFlatOnly, S_.ArbHierOnly, Asym);
								}
								else
								{
									UE_LOG(LogVoxelMarch, Display,
									       TEXT("      GATE v4: FAIL -- flat-only %u vs "
									            "hier-only %u, asymmetry %.2fx over the 1.5x "
									            "bar. A directional skip defect remains. At "
									            "level 0 this is ~1e-5 of the frame; A SKIPPED "
									            "BRICK IS 0.8 m HERE AND 25.6 m AT LEVEL 5, so "
									            "re-run this gate on every level the cascade "
									            "adds before judging it harmless there."),
									       S_.ArbFlatOnly, S_.ArbHierOnly, Asym);
								}
							}
						}
						// CONSERVATION FIRST. If the arbiter did not classify the
						// same rays the shape terms counted, every comparison below
						// is between two different populations.
						{
							const uint32 Shape = S_.HvfLost + S_.HvfGained + S_.HvfMoved;
							if (S_.ArbTotal != Shape)
							{
								UE_LOG(LogVoxelMarch, Warning,
								       TEXT("      ARBITER: POPULATION MISMATCH -- classified "
								            "%u rays but lost+gained+moved is %u. The arbiter "
								            "is not looking at the disagreements. Nothing "
								            "below is comparable; fix this first."),
								       S_.ArbTotal, Shape);
							}
							// The six outcomes must exhaust the classified rays. A
							// slot mistake shows up here and nowhere else.
							const uint32 Sum = S_.ArbFlat + S_.ArbHier + S_.ArbTie +
							                   S_.ArbNeither + S_.ArbFlatOnly + S_.ArbHierOnly;
							if (Sum != S_.ArbTotal)
							{
								UE_LOG(LogVoxelMarch, Warning,
								       TEXT("      ARBITER: OUTCOMES DO NOT SUM -- %u across "
								            "six bins against %u classified. A slot is "
								            "mis-numbered."),
								       Sum, S_.ArbTotal);
							}
							// The uncontested bins are bounded by their shape classes
							// by construction. If hier-only ever exceeds `gained`,
							// the uncontested split is picking up contested rays --
							// which is the defect this round removed, returning.
							if (S_.ArbHierOnly > S_.HvfGained || S_.ArbFlatOnly > S_.HvfLost)
							{
								UE_LOG(LogVoxelMarch, Warning,
								       TEXT("      ARBITER: UNCONTESTED OVERFLOW -- hier-only "
								            "%u vs gained %u, flat-only %u vs lost %u. An "
								            "uncontested bin cannot exceed its shape class."),
								       S_.ArbHierOnly, S_.HvfGained, S_.ArbFlatOnly, S_.HvfLost);
							}
						}
						// WHY IT COULD NOT DECIDE. Printed before the verdict,
						// because on three legs of four this is the result and the
						// verdict is noise.
						if (S_.ArbNeither > 0u)
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      ARBFAIL side A (flat cand): ok=%u nocand=%u "
							            "degenerate=%u beforeTIn=%u afterTOut=%u notsolid=%u"),
							       S_.ArbFailA[0], S_.ArbFailA[1], S_.ArbFailA[2],
							       S_.ArbFailA[3], S_.ArbFailA[4], S_.ArbFailA[5]);
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      ARBFAIL side B (hier cand): ok=%u nocand=%u "
							            "degenerate=%u beforeTIn=%u afterTOut=%u notsolid=%u"),
							       S_.ArbFailB[0], S_.ArbFailB[1], S_.ArbFailB[2],
							       S_.ArbFailB[3], S_.ArbFailB[4], S_.ArbFailB[5]);
							if (S_.ArbFailA[5] > 0u)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      ARBFAIL A probe reason: solid=%u nochunk=%u "
								            "record=%u l1clear=%u kindair=%u occclear=%u "
								            "bounds=%u"),
								       S_.ArbFailAReason[0], S_.ArbFailAReason[1],
								       S_.ArbFailAReason[2], S_.ArbFailAReason[3],
								       S_.ArbFailAReason[4], S_.ArbFailAReason[5],
								       S_.ArbFailAReason[6]);
								if (S_.ArbFailAReason[0] > 0u)
								{
									UE_LOG(LogVoxelMarch, Warning,
									       TEXT("      ARBFAIL: PROBE IS NOT DETERMINISTIC -- "
									            "%u rays classed not-solid whose recorded "
									            "reason is SOLID. Two reads of one storage "
									            "disagreeing; that is the bug, not the walk."),
									       S_.ArbFailAReason[0]);
								}
							}
							FString BandRow;
							for (int32 i = 0; i < FVoxelMarchStats::FSourceCompare::kBandBins; ++i)
							{
								BandRow += FString::Printf(TEXT("%u "), S_.ArbFailBand[i]);
							}
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      ARBFAIL by band: %s"), *BandRow);
							// The aimed conclusion. Degenerate dominating means the
							// arbiter's own strict inequality is rejecting the
							// grazing rays it exists to judge -- self-inflicted, and
							// the fix is a tolerance from the 8-ulp band already
							// derived, not a new threshold.
							const uint32 Deg = S_.ArbFailA[2] + S_.ArbFailB[2];
							const uint32 NotS = S_.ArbFailA[5] + S_.ArbFailB[5];
							const uint32 SegLo = S_.ArbFailA[3] + S_.ArbFailB[3];
							const uint32 SegHi = S_.ArbFailA[4] + S_.ArbFailB[4];
							const uint32 Seg = SegLo + SegHi;
							// SEGMENT IS NOT A FAILURE any more -- it no longer
							// invalidates a candidate, so it must not sit in the
							// denominator of a verdict about failures.
							(void)Seg;
							const uint32 FailTot = Deg + NotS;
							// Reported beside the failures, never inside them. The
							// MAGNITUDE is the question: 8-ulp scale (~5e-3 UU here)
							// is the arbiter's own precision meeting the walk's
							// accumulated drift, which grows with path length and is
							// why this collapsed in short legs and grew to 1,221 in
							// the long one. VOXEL scale (10 UU) would instead mean a
							// walk returned a hit outside the segment it was given,
							// which is a contract violation and a real bug.
							if (Seg > 0u)
							{
								const bool bVoxelScale = S_.SegHiMaxOverUU >= 1.0f;
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      SEGMENT (diagnostic, non-invalidating): "
								            "beforeTIn=%u afterTOut=%u | max overshoot past "
								            "TOut %.5f UU (%.4f voxels) -- %s"),
								       SegLo, SegHi, S_.SegHiMaxOverUU,
								       S_.SegHiMaxOverUU / 10.0f,
								       bVoxelScale
								           ? TEXT("VOXEL SCALE: a walk returned a hit outside "
								                  "its own segment. Contract violation, not "
								                  "drift.")
								           : TEXT("float scale: accumulated walk drift, grows "
								                  "with path length. Not a correctness fault "
								                  "and NOT to be absorbed by widening the "
								                  "arbiter's tolerance."));
							}
							// MINIMUM COUNT BEFORE A PROPORTIONAL VERDICT. The same
							// guard gate v4 already carries, and it was missing here:
							// "14 of 20 degenerate" reads as domination, but 14 is
							// within Poisson noise of 10 and the absolute degenerate
							// count is 14/11/21/21 -- essentially CONSTANT across legs
							// whose other terms span 695x. A share only separates from
							// 50% at these effect sizes once the denominator reaches
							// ~50; below that the 95% interval on 70% still covers it.
							if (FailTot > 0u && FailTot < 50u)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      ARBFAIL VERDICT: NO VERDICT -- only %u "
								            "failures (degenerate=%u notsolid=%u segment=%u). "
								            "Too few to call a dominant cause; a 70%% share of "
								            "20 is not distinguishable from half. Read the "
								            "ABSOLUTE counts across legs instead."),
								       FailTot, Deg, NotS, Seg);
							}
							else if (FailTot > 0u && Deg * 2u >= FailTot)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      ARBFAIL VERDICT: SELF-INFLICTED -- %u of %u "
								            "failures are a degenerate slab span. The arbiter "
								            "rejects near-grazing rays, which is exactly the "
								            "population it exists to judge. The 8-ulp span "
								            "tolerance is APPLIED as of 2026-08-20; if this "
								            "still fires, the tolerance is not the whole cause."),
								       Deg, FailTot);
							}
							else if (FailTot > 0u && NotS * 2u >= FailTot)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      ARBFAIL VERDICT: STORAGE JOIN -- %u of %u "
								            "failures are the pool refusing a voxel a walk "
								            "hit. Two readers of one storage disagree; the "
								            "probe reason above names which read."),
								       NotS, FailTot);
							}
							else if (FailTot > 0u)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      ARBFAIL VERDICT: MIXED -- degenerate=%u "
								            "notsolid=%u segment=%u (before TIn %u / after "
								            "TOut %u), no majority. Do not fit one cause."),
								       Deg, NotS, Seg, SegLo, SegHi);
							}
						}
						if (Tot == 0u)
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      ARBITER: no verdict -- no disagreeing rays."));
						}
						else
						{
							const double TiePct  = 100.0 * double(S_.ArbTie) / double(Tot);
							const double FlatPct = 100.0 * double(S_.ArbFlat) / double(Tot);
							const double HierPct = 100.0 * double(S_.ArbHier) / double(Tot);
							const double IllPct  = 100.0 * double(S_.ArbNeither) / double(Tot);
							// Bars set before any number existed, and accepted in
							// advance by the coordinator so neither outcome gets
							// relitigated after it lands.
							if (IllPct >= 25.0)
							{
								UE_LOG(LogVoxelMarch, Warning,
								       TEXT("      ARBITER: NO VERDICT -- %.1f%% ill-posed. "
								            "The winning candidate is not solid in the pool "
								            "or not on the ray, so the COMPARISON is faulty, "
								            "not the walks. Fix that before reading the rest."),
								       IllPct);
							}
							else if (TiePct >= 50.0)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      ARBITER: RESOLUTION LIMIT -- %.1f%% of "
								            "disagreements are genuinely ambiguous at float "
								            "precision. NEITHER WALK IS WRONG. Certification "
								            "as currently defined is unachievable; redefine "
								            "the gate against the arbiter and stop."),
								       TiePct);
							}
							else if (FlatPct >= 50.0)
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      ARBITER: REAL DEFECT IN THE HIERARCHY -- "
								            "%.1f%% of disagreements are rays that genuinely "
								            "enter the flat walk's solid voxel first. A fix "
								            "now has a reference that is not a float walk "
								            "assuming its own correctness."),
								       FlatPct);
							}
							else if (HierPct >= 50.0)
							{
								UE_LOG(LogVoxelMarch, Warning,
								       TEXT("      ARBITER: THE REFERENCE IS WRONG -- %.1f%% "
								            "of disagreements are rays the HIERARCHY gets "
								            "right and the FLAT walk gets wrong. Every gate "
								            "built on flat-as-truth needs restating."),
								       HierPct);
							}
							else
							{
								UE_LOG(LogVoxelMarch, Display,
								       TEXT("      ARBITER: MIXED -- flat %.1f%% hier %.1f%% "
								            "tie %.1f%% ill-posed %.1f%%, no majority. More "
								            "than one cause is present; do not fit a single "
								            "mechanism to this."),
								       FlatPct, HierPct, TiePct, IllPct);
							}
						}
					}
					{
						const uint32 OnFace = S_.LostOnFace + S_.MovedOnFace;
						const uint32 OffFace = S_.LostOffFace + S_.MovedOffFace;
						if (OnFace > 0u && OnFace >= OffFace * 3u)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      THE DEFECT IS AT BRICK BOUNDARIES: %u on-face "
							            "against %u off-face. Drive the on-face term to zero; "
							            "the off-face term is the residue to characterise "
							            "against refNoise=%u."),
							       OnFace, OffFace, S_.RefNoise);
						}
						else if (OffFace > 0u && OffFace >= OnFace * 3u)
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      the remainder is OFF brick boundaries (%u vs %u). "
							            "The boundary handoff is not the mechanism here -- look "
							            "at stepping inside a brick."),
							       OffFace, OnFace);
						}
					}

					// ---- CONSERVATION, CHECKED BY THE INSTRUMENT ITSELF -----
					//
					// Two lines of this report were read as contradicting each
					// other within one leg (`moved onFace=0 offFace=0` beside
					// `moved by delta` counting 173). The counters are
					// incremented on ADJACENT LINES of the same branch and the
					// decode indices match the shader, so that cannot happen in
					// one readback -- but nothing in the output could say so, and
					// "which line do I believe" is not a question a reader should
					// have to answer.
					//
					// Every split must sum to its parent. If one does not, the
					// decode is misaligned with the shader's slot layout -- which
					// has shifted four times as counters were added -- and EVERY
					// number here is suspect, not just the two that disagree.
					{
						const uint32 Parts = C.HvfLost + C.HvfGained + C.HvfMoved;
						const uint32 LostSplit = C.LostOnFace + C.LostOffFace;
						const uint32 LostDesc = C.Descend[0] + C.Descend[1] + C.Descend[2];
						const uint32 MovedSplit = C.MovedOnFace + C.MovedOffFace;
						const uint32 MovedDelta =
							C.MovedDelta[0] + C.MovedDelta[1] + C.MovedDelta[2];
						const bool bOk = (Parts == C.HierVsFlat) && (LostSplit == C.HvfLost) &&
						                 (LostDesc == C.HvfLost) && (MovedSplit == C.HvfMoved) &&
						                 (MovedDelta == C.HvfMoved);
						if (!bOk)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      COUNTER CONSERVATION FAILED -- DO NOT READ THE "
							            "NUMBERS ABOVE. lost+gained+moved=%u vs disagree=%u | "
							            "lost face-split=%u descent-split=%u vs lost=%u | "
							            "moved face-split=%u delta-split=%u vs moved=%u. The "
							            "readback decode is misaligned with the shader's slot "
							            "layout."),
							       Parts, C.HierVsFlat, LostSplit, LostDesc, C.HvfLost,
							       MovedSplit, MovedDelta, C.HvfMoved);
						}
					}

					// ---- DESCENT AND DELTA, OVER THE WHOLE POPULATION -------
					//
					// HOISTED OUT OF THE SAMPLE BLOCK. These were computed for
					// every ray in the shader and then printed only inside
					// `if (HvfSampleCount > 0)`, several screens below the counts
					// -- so three legs were read from ONE EXAMPLE LINE EACH while
					// the exact split sat unprinted. An aggregate that is hard to
					// find is an aggregate nobody uses.
					if (S_.HvfLost > 0u)
					{
						const uint32 D0 = S_.Descend[0], D1 = S_.Descend[1], D2 = S_.Descend[2];
						const uint32 DTot = D0 + D1 + D2;
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      lost by descent (exact, all %u rays): "
						            "neverDescended=%u descendedAndMissed=%u inconclusive=%u"),
						       DTot, D0, D1, D2);
						const uint32 DBar = (DTot * 3u + 3u) / 4u;
						if (DTot > 0u && D0 >= DBar)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      THE HIT'S BRICK WAS NEVER WALKED -- the advance "
							            "skips past it. Look at the advance, not the inner DDA."));
						}
						else if (DTot > 0u && D1 >= DBar)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      THE BRICK WAS WALKED AND THE INNER DDA MISSED. A "
							            "stepping defect, distinct from every advance bug fixed "
							            "so far."));
						}
						else if (DTot > 0u && D2 >= DBar)
						{
							UE_LOG(LogVoxelMarch, Warning,
							       TEXT("      MOSTLY INCONCLUSIVE: the descent mask covers only "
							            "the last chunk walked and the hit is elsewhere. Widen it "
							            "before it can arbitrate."));
						}
					}
					// PRINTED EVEN AT ZERO, and labelled with its parent count.
					//
					// This line used to be suppressed when moved==0. A reader
					// then saw the previous leg's line and attributed it to this
					// one -- which is exactly what happened: r6's "all 173 rays"
					// was read beside r7's "moved onFace=0 offFace=0" and looked
					// like a contradiction inside one leg. A line that disappears
					// is a line that gets carried forward.
					{
						const uint32 M0 = S_.MovedDelta[0], M1 = S_.MovedDelta[1],
						             M2 = S_.MovedDelta[2];
						const uint32 MTot = M0 + M1 + M2;
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      moved by delta (%u of %u moved this leg): aheadByOne=%u "
						            "behindByOne=%u other=%u"),
						       MTot, S_.HvfMoved, M0, M1, M2);
						const uint32 MBar = (MTot * 3u + 3u) / 4u;
						if (MTot > 0u && M0 >= MBar)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      OFF-BY-ONE AT ENTRY: the hierarchy lands exactly "
							            "one voxel FURTHER along the ray. Consistent with the "
							            "fine walk starting a nudge past the boundary and missing "
							            "the first voxel."));
						}
						else if (MTot > 0u && M1 >= MBar)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      OFF-BY-ONE BEHIND: the hierarchy lands one voxel "
							            "NEARER than the flat walk -- the mirror case, and a "
							            "different mechanism from an entry overshoot."));
						}
						else if (MTot > 0u && M2 >= MBar)
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      NOT an off-by-one: the deltas scatter. A stepping "
							            "error inside a brick rather than at its entry."));
						}
					}

					// THE DUMP. Brick coordinates are the voxel >> 3, printed
					// beside the voxels so the two candidate failures are read
					// off directly rather than inferred from a tally.
					static const TCHAR* HvfClassNames[3] = {
						TEXT("lost"), TEXT("gained"), TEXT("moved")
					};
					// THE `lost` POPULATION, over all of it rather than six samples.
					static const TCHAR* TermNames[4] = {
						TEXT("hit"), TEXT("segment-end"), TEXT("budget"), TEXT("chunk-cap")
					};
					static const TCHAR* ProbeNames[7] = {
						TEXT("POOL-SAYS-SOLID"), TEXT("no-chunk"), TEXT("record"), TEXT("L1-clear"),
						TEXT("kind-air"), TEXT("occ-clear"), TEXT("bounds")
					};
					if (S_.HvfLost > 0u)
					{
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      lost by termination: segment-end=%u budget=%u "
						            "chunk-cap=%u | lost by probe of the flat hit voxel: "
						            "solid=%u noChunk=%u record=%u L1=%u kindAir=%u occClear=%u "
						            "bounds=%u"),
						       S_.LostTerm[1], S_.LostTerm[2], S_.LostTerm[3], S_.LostProbe[0],
						       S_.LostProbe[1], S_.LostProbe[2], S_.LostProbe[3], S_.LostProbe[4],
						       S_.LostProbe[5], S_.LostProbe[6]);
						if (S_.LostTerm[2] * 2u >= S_.HvfLost)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      MOST LOST RAYS RAN OUT OF STEP BUDGET. Neither a "
							            "skip nor an under-run -- the walk never reached the "
							            "voxel, and every count taken so far would show this as "
							            "a skip."));
						}
						else if (S_.LostProbe[0] * 2u >= S_.HvfLost)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      MOST LOST RAYS PASSED A VOXEL THE POOL CALLS "
							            "SOLID. The data is there and the walk stepped over it: "
							            "a genuine skip, and a different bug from the containment "
							            "family."));
						}
						else if ((S_.LostProbe[3] + S_.LostProbe[4] + S_.LostProbe[5]) * 2u >=
						         S_.HvfLost)
						{
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      MOST LOST RAYS HIT A VOXEL THE BRICK PATH CALLS "
							            "AIR while the flat walk called it solid -- yet both read "
							            "the same arenas. That contradicts six clean source "
							            "compares and points at the L1/descriptor read, not the "
							            "walk."));
						}
					}
					// ---- THE SUMMARY, WHICH DOES THE PATTERN-MATCHING -------
					//
					// Sixty-four printed lines is a haystack. What certification
					// needs is whether the disagreeing rays SHARE a property, and
					// a human scanning coordinates is exactly the step this
					// project has been burned by. So the histogram is computed
					// here and the conclusion stated.
					if (S_.HvfSampleCount > 0)
					{
						int32 AxisHist[3] = {};
						int32 SignHist[2] = {};
						int32 OnBrickFace = 0;
						int32 CellMin[3] = {7, 7, 7};
						int32 CellMax[3] = {0, 0, 0};
						for (int32 i = 0; i < S_.HvfSampleCount; ++i)
						{
							const auto& M = S_.HvfSamples[i];
							AxisHist[FMath::Min(M.FlatFace & 3u, 2u)]++;
							SignHist[(M.FlatFace & 4u) ? 1 : 0]++;
							const int32 Cell[3] = { int32(M.FlatCellInBrick & 7u),
							                        int32((M.FlatCellInBrick >> 3) & 7u),
							                        int32((M.FlatCellInBrick >> 6) & 7u) };
							bool bFace = false;
							for (int32 a = 0; a < 3; ++a)
							{
								CellMin[a] = FMath::Min(CellMin[a], Cell[a]);
								CellMax[a] = FMath::Max(CellMax[a], Cell[a]);
								if (Cell[a] == 0 || Cell[a] == 7) { bFace = true; }
							}
							OnBrickFace += bFace ? 1 : 0;
						}
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      shared properties over %d dumped rays: face axis "
						            "x=%d y=%d z=%d | sign -=%d +=%d | on a brick face=%d of %d | "
						            "cell range x[%d..%d] y[%d..%d] z[%d..%d]"),
						       S_.HvfSampleCount, AxisHist[0], AxisHist[1], AxisHist[2],
						       SignHist[0], SignHist[1], OnBrickFace, S_.HvfSampleCount,
						       CellMin[0], CellMax[0], CellMin[1], CellMax[1], CellMin[2],
						       CellMax[2]);
						// ---- THE VERDICT, AND WHY IT IS A THRESHOLD ----------
						//
						// This tested for EVERY and therefore reported nothing when
						// the answer was 89%: 57 of 64 rays on the +Y face and 59
						// of 64 on a brick boundary, printed under the words "no
						// single shared property". The instrument found the
						// mechanism and its own conclusion hid it.
						//
						// EVERY is the right bar for declaring CERTAINTY and the
						// wrong bar for declaring PRESENCE. A structural defect
						// with a handful of exceptions is still a structural
						// defect -- and by requiring uniformity it read identically
						// to noise, which is the same failure shape as a check
						// whose stated conclusion is narrower than its own data.
						//
						// 75%: well above the ~33% a uniform distribution over
						// three axes would give, and low enough that a mechanism
						// with a fifth of its rays elsewhere still reports.
						const int32 N = S_.HvfSampleCount;
						const int32 Bar = (N * 3 + 3) / 4;   // ceil(0.75 * N)
						const int32 BestAxis =
							(AxisHist[0] >= AxisHist[1] && AxisHist[0] >= AxisHist[2]) ? 0
							: ((AxisHist[1] >= AxisHist[2]) ? 1 : 2);
						static const TCHAR* AxisNames[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
						const int32 BestSign = (SignHist[1] >= SignHist[0]) ? 1 : 0;
						bool bConcentrated = false;
						if (AxisHist[BestAxis] >= Bar)
						{
							bConcentrated = true;
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      CONCENTRATED ON ONE AXIS: %d of %d rays (%.0f%%) "
							            "hit a %s face, %d of %d (%.0f%%) with sign %s. A "
							            "STRUCTURAL asymmetry, not a float floor -- a face "
							            "handoff that is not symmetric between the two "
							            "directions of that axis."),
							       AxisHist[BestAxis], N, 100.0 * AxisHist[BestAxis] / N,
							       AxisNames[BestAxis], SignHist[BestSign], N,
							       100.0 * SignHist[BestSign] / N,
							       BestSign ? TEXT("+") : TEXT("-"));
						}
						// SIGN, TESTED ON ITS OWN. It used to be REPORTED only
						// inside the axis branch, so a run with 53 of 64 rays
						// (83%) sharing a sign printed nothing because its best
						// AXIS was 45% and the branch never ran. A check that
						// would have fired and did not -- the same failure this
						// whole verdict block was rewritten to remove, left in
						// place one nesting level down.
						//
						// Sign is independent of axis and has its own meaning: a
						// concentration here says the defect distinguishes the two
						// DIRECTIONS of travel, whatever plane is crossed.
						if (SignHist[BestSign] >= Bar)
						{
							bConcentrated = true;
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      CONCENTRATED ON ONE DIRECTION: %d of %d rays "
							            "(%.0f%%) entered moving %s along their hit axis. The "
							            "defect distinguishes direction of travel, independently "
							            "of which plane is crossed."),
							       SignHist[BestSign], N, 100.0 * SignHist[BestSign] / N,
							       BestSign ? TEXT("positively") : TEXT("negatively"));
						}
						if (OnBrickFace >= Bar)
						{
							bConcentrated = true;
							UE_LOG(LogVoxelMarch, Error,
							       TEXT("      CONCENTRATED ON BRICK BOUNDARIES: %d of %d rays "
							            "(%.0f%%) sit at cell 0 or 7. The defect is in the "
							            "handoff between the brick loop and the fine walk, not "
							            "in the stepping inside either."),
							       OnBrickFace, N, 100.0 * OnBrickFace / N);
						}
						if (!bConcentrated)
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      no property reaches %d of %d (75%%). Best: axis %s "
							            "%d, sign %s %d, on-face %d. Diffuse at a small count is "
							            "consistent with the restart float floor -- read it "
							            "against refNoise, not against zero."),
							       Bar, N, AxisNames[BestAxis], AxisHist[BestAxis],
							       BestSign ? TEXT("+") : TEXT("-"), SignHist[BestSign],
							       OnBrickFace);
						}
					}
					bool bAnyBrickMismatch = false;
					for (int32 i = 0; i < S_.HvfSampleCount; ++i)
					{
						const auto& M = S_.HvfSamples[i];
						const FIntVector FB(M.FlatVoxel.X >> 3, M.FlatVoxel.Y >> 3,
						                    M.FlatVoxel.Z >> 3);
						const FIntVector HB(M.HierVoxel.X >> 3, M.HierVoxel.Y >> 3,
						                    M.HierVoxel.Z >> 3);
						// CLASS 2 (moved) ONLY. This read `Class != 0`, which
						// excluded `lost` -- where the HIER voxel is absent -- and
						// forgot `gained`, where the FLAT voxel is absent. The flat
						// walk leaves H.Voxel = StartVoxel on a miss, a perfectly
						// legal-looking coordinate, so every gained sample compared
						// a real brick against the ray's ENTRY brick and the
						// assertion fired. `moved` is the only class where both
						// sides are real hits and a brick comparison means anything.
						if (M.Class == 2u && FB != HB) { bAnyBrickMismatch = true; }
						if (M.Class == 0u)
						{
							// lost: the hierarchy has no hit voxel, so print where
							// it STOPPED instead of a meaningless brick compare.
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      lost px(%d,%d): flat voxel (%d,%d,%d) brick "
							            "(%d,%d,%d) t=%.4f | hier term=%s steps=%u lastChunk "
							            "(%d,%d,%d) lastBrick=%d | probe(flat voxel)=%s"),
							       M.Pixel.X, M.Pixel.Y, M.FlatVoxel.X, M.FlatVoxel.Y,
							       M.FlatVoxel.Z, FB.X, FB.Y, FB.Z, M.FlatT,
							       TermNames[FMath::Min(M.TermReason, 3u)], M.HierSteps,
							       M.LastChunk.X, M.LastChunk.Y, M.LastChunk.Z, M.LastBrick,
							       ProbeNames[FMath::Min(M.ProbeReason, 6u)]);
							// THE LINE THAT LOCALISES IT. The flat hit's brick
							// index within its chunk, against the brick the
							// hierarchy actually descended into, against the t
							// it descended at versus the t of the hit.
							const int32 FlatBrickIdx =
								((M.FlatVoxel.X >> 3) & 3) +
								4 * (((M.FlatVoxel.Y >> 3) & 3) +
								     4 * ((M.FlatVoxel.Z >> 3) & 3));
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("            flatBrickIdx=%d | descended into brick=%d "
							            "at t=%.4f (hit t=%.4f, %s) | fineWalks=%u"),
							       FlatBrickIdx, M.LastFineBrick, M.LastFineT, M.FlatT,
							       M.LastFineBrick != FlatBrickIdx
							           ? TEXT("NEVER DESCENDED INTO THE HIT'S BRICK")
							           : (M.LastFineT > M.FlatT
							                  ? TEXT("ENTERED PAST THE HIT")
							                  : TEXT("entered before the hit -- inner DDA missed")),
							       M.FineWalks);
						}
						else if (M.Class == 1u)
						{
							// gained: THE FLAT VOXEL IS NOT A POSITION. The flat walk
							// leaves H.Voxel = the ray's entry voxel on a miss, with
							// t = 0, so printing it invited exactly the reading it
							// got -- three pixels "hitting" one coordinate at t=0.
							// Say there is no candidate instead of showing a fake one.
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      gained px(%d,%d): flat NO HIT (no candidate; "
							            "the flat voxel field holds the ray's entry voxel, "
							            "not a position) | hier voxel (%d,%d,%d) brick "
							            "(%d,%d,%d) t=%.4f"),
							       M.Pixel.X, M.Pixel.Y,
							       M.HierVoxel.X, M.HierVoxel.Y, M.HierVoxel.Z,
							       HB.X, HB.Y, HB.Z, M.HierT);
						}
						else
						{
							UE_LOG(LogVoxelMarch, Display,
							       TEXT("      %s px(%d,%d): flat voxel (%d,%d,%d) brick "
							            "(%d,%d,%d) t=%.4f | hier voxel (%d,%d,%d) brick "
							            "(%d,%d,%d) t=%.4f"),
							       HvfClassNames[FMath::Min(M.Class, 2u)], M.Pixel.X, M.Pixel.Y,
							       M.FlatVoxel.X, M.FlatVoxel.Y, M.FlatVoxel.Z, FB.X, FB.Y, FB.Z,
							       M.FlatT, M.HierVoxel.X, M.HierVoxel.Y, M.HierVoxel.Z,
							       HB.X, HB.Y, HB.Z, M.HierT);
						}
					}
					if (bAnyBrickMismatch)
					{
						UE_LOG(LogVoxelMarch, Error,
						       TEXT("      BRICKS DIFFER between the two walks. Containment is "
						            "now decided in integer voxel space (voxel >> 3), so this "
						            "cannot happen inside the fine walk -- some path is still "
						            "reading one brick's occupancy for a voxel in another."));
					}
					else if (S_.HvfSampleCount > 0)
					{
						UE_LOG(LogVoxelMarch, Display,
						       TEXT("      bricks AGREE on every sample: the walk descends into "
						            "the right brick and stops on the wrong cell. A stepping "
						            "error, not an aliasing one."));
					}
				}
				if (S_.HierVsFlat > S_.RefNoise)
				{
					UE_LOG(LogVoxelMarch, Error,
					       TEXT("    THE RATIO IS NOT USABLE: the hierarchical walk disagrees with "
					            "the flat walk on %u rays against a noise floor of %u. A hierarchy "
					            "that skips a diagonal cell produces a LOWER step count and a "
					            "plausible picture -- it makes this number BETTER while being "
					            "wrong. Fix the walk before quoting the ratio."),
					       S_.HierVsFlat, S_.RefNoise);
				}
				else if (Ratio < 4.0)
				{
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("    %.2fx is materially below the 9.19x the plan's projections "
					            "assume. That figure came from a two-level mip over a FLAT volume "
					            "and was always indicative rather than predictive. THIS IS A "
					            "FINDING ABOUT THE PROJECT, not a defect -- surface it before "
					            "anything downstream is planned on the old number."),
					       Ratio);
				}
			}

			static const TCHAR* ReasonNames[7] = {
				TEXT("POOL-SAYS-SOLID"), TEXT("no-chunk"), TEXT("record-reject"),
				TEXT("L1-clear"), TEXT("kind-air"), TEXT("occ-clear"), TEXT("bounds")
			};
			// GATED, because these belong to the OCCUPANCY-vs-brick arm. On a
			// camera-anchored frame that arm does not run, and this printed seven
			// zeroes beside live disagreements -- which reads as an instrument
			// finding nothing rather than an instrument that was never asked.
			// The hier-vs-flat miss reasons are a DIFFERENT set (LostProbe) and
			// they print above, live, with their own verdict.
			if (C.bOccValid)
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("  why the brick walk missed: solid=%u noChunk=%u record=%u "
				            "L1=%u kindAir=%u occClear=%u bounds=%u"),
				       C.Reasons[0], C.Reasons[1], C.Reasons[2], C.Reasons[3], C.Reasons[4],
				       C.Reasons[5], C.Reasons[6]);
			}
			else
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("  why the brick walk missed: NOT ASKED -- these counters "
				            "belong to the occupancy arm, which is dark on this frame. "
				            "Zeroes here would mean nothing; see the lost-probe line above "
				            "for the live miss reasons."));
			}
			for (int32 i = 0; i < C.SampleCount; ++i)
			{
				const FVoxelMarchStats::FSourceCompare::FSample& Sm = C.Samples[i];
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    sample %d px(%d,%d): occupancy hit voxel (%d,%d,%d) at t=%.3f UU; "
				            "brick walk ended (%d,%d,%d) after %u steps; reason=%s"),
				       i, Sm.Pixel.X, Sm.Pixel.Y, Sm.OccVoxel.X, Sm.OccVoxel.Y, Sm.OccVoxel.Z,
				       Sm.OccT, Sm.BrickVoxel.X, Sm.BrickVoxel.Y, Sm.BrickVoxel.Z,
				       Sm.BrickSteps, ReasonNames[FMath::Min(Sm.Reason, 6u)]);
			}
			// THE CONCLUSION, DRAWN RATHER THAN LEFT TO THE READER -- the shape
			// that resolved the last two legs in one run each.
			if (C.BrickMissed > 0u && C.Reasons[0] * 2u >= C.BrickMissed)
			{
				UE_LOG(LogVoxelMarch, Error,
				       TEXT("  MOST MISSED VOXELS ARE ONES THE POOL CALLS SOLID. The storage holds "
				            "them and the walk never tested them: the DDA is skipping voxels. "
				            "Compare the sample voxels against the brick walk's end point -- the "
				            "axis they differ on is the axis being skipped."));
			}
			else if (C.BrickMissed > 0u && (C.Reasons[1] + C.Reasons[2]) * 2u >= C.BrickMissed)
			{
				UE_LOG(LogVoxelMarch, Error,
				       TEXT("  MOST MISSES LAND IN CHUNKS THE INDEX DOES NOT COVER. The walk is "
				            "looking outside the resident set -- a coordinate-frame error, not a "
				            "stepping one. Check MarchBrickOriginVoxel against the occupancy "
				            "origin the same rays were built from."));
			}
			else if (C.BrickMissed > 0u)
			{
				UE_LOG(LogVoxelMarch, Error,
				       TEXT("  THE STORAGE DISAGREES AT THOSE EXACT VOXELS, which contradicts the "
				            "layer-2 aggregate (poolSolidCells > occupancySolidVoxels over the "
				            "same region). The disagreement is positional, not about content."));
			}
		}

		// The join probe.
		const FVoxelMarchStats::FIndexProbe& P = S.IndexProbe;
		if (P.bValid)
		{
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("Voxel march index probe: probed=%u resident=%u | valid=%u "
			            "(L1any=%u L1zero=%u) | rejects: origin=%u level=%u anySolidClear=%u "
			            "slotOOB=%u brickBaseOOB=%u"),
			       P.Probed, P.Resident, P.Valid, P.L1Any, P.L1Zero, P.OriginMismatch,
			       P.LevelMismatch, P.AnySolidClear, P.SlotOutOfRange, P.BrickBaseOutOfRange);
			// LAYER 2, AND IT IS THE LINE THAT LOCALISES THE BUG.
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("  storage: poolSolidCells=%u vs occupancySolidVoxels=%u (ratio %.3f) | "
			            "L1-set bricks decode as air=%u solid=%u mixed=%u"),
			       P.PoolSolidCells, P.OccSolidVoxels,
			       P.OccSolidVoxels > 0u ? double(P.PoolSolidCells) / double(P.OccSolidVoxels)
			                             : 0.0,
			       P.KindAir, P.KindSolid, P.KindMixed);
			if (P.KindAir > 0u)
			{
				UE_LOG(LogVoxelMarch, Error,
				       TEXT("    %u bricks have their L1 bit SET but decode as uniform AIR. That "
				            "is a contradiction the storage cannot legitimately produce -- the "
				            "writer only sets an L1 bit for a brick with solid content. The "
				            "descriptor READ is wrong, not the data."),
				       P.KindAir);
			}
			else if (P.OccSolidVoxels > 0u && P.PoolSolidCells * 2u < P.OccSolidVoxels)
			{
				UE_LOG(LogVoxelMarch, Error,
				       TEXT("    the pool reports less than half the solid cells the occupancy "
				            "volume does over the SAME voxels, yet the render predicate is a "
				            "SUPERSET of the fluid one. The occupancy-dword read is wrong."));
			}
			else if (P.PoolSolidCells >= P.OccSolidVoxels && P.OccSolidVoxels > 0u)
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    the storage holds the same world, read through the marcher's own "
				            "path. The defect is in the DDA, not in the lookup or the decode."));
			}
			for (int32 i = 0; i < P.SampleCount; ++i)
			{
				const FVoxelMarchStats::FIndexProbe::FSample& Sm = P.Samples[i];
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("    sample %d: wanted origin (%d,%d,%d) got (%d,%d,%d) "
				            "slot=%u levelAndFlags=0x%08x"),
				       i, Sm.Want.X, Sm.Want.Y, Sm.Want.Z, Sm.Got.X, Sm.Got.Y, Sm.Got.Z,
				       Sm.Slot, Sm.LevelAndFlags);
			}
			if (P.Resident > 0u && P.Valid == 0u)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("    every resident cell was REJECTED. The join is broken, not the "
				            "traversal -- read the samples above before touching the DDA."));
			}
			else if (P.Valid > 0u && P.L1Any == 0u)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("    the join is clean but every valid chunk has an EMPTY L1 mask. "
				            "The bug is below the join, in the brick layer."));
			}
		}

		// THE TWO READING RULES, PRINTED WITH THE NUMBERS RATHER THAN LEFT IN A
		// HEADER NOBODY HAS OPEN AT 2 A.M.
		if (S.Frames != S.EmitFrames)
		{
			UE_LOG(LogVoxelMarch, Warning,
			       TEXT("  frames != emitFrames: the emit declined on some frames, so RDG had "
			            "no consumer for the march output and may have culled it. marchMs may "
			            "describe work that did not run. Do not quote it."));
		}
		if (Arm.Mode == 2 && !S.bHTileProbe)
		{
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("  emitMs is the EMIT PASS's own cost, NOT the HTILE bill. In mode 2 the "
			            "emit writes scratch depth, so the real SceneDepth's metadata is never "
			            "invalidated and the downstream decompression cost is structurally zero "
			            "here. For that number: voxel.March.HTileProbe 1 vs 0, same pose, and "
			            "read the FRAME time -- the difference is the bill. Budget 0.2-0.4 ms."));
		}
		if (Arm.Mode == 2)
		{
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("  mode2frame - mode0frame should equal marchMs + emitMs + scratchMs. "
			            "scratchMs is the seven full-screen clears mode 2 adds and the control "
			            "does not pay; it is not marcher cost. If the identity does not close, "
			            "something is being paid that no bracket here contains."));
		}

		// THE GATE, printed as a verdict AND as its ingredients. The verdict is
		// computed against the criterion pre-registered in the header, not
		// against whatever the numbers happen to be -- and the ingredients are
		// printed beside it so the verdict can be checked rather than believed.
		const FVoxelMarchStats::FDepthGate& G = S.DepthGate;
		if (!G.bValid)
		{
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("Voxel march depth gate: NOT RUN (voxel.March.VerifyDepth 0, or no "
			            "readback has landed yet). This is not a pass."));
		}
		else if (G.Compared == 0)
		{
			UE_LOG(LogVoxelMarch, Warning,
			       TEXT("Voxel march depth gate: ZERO PIXELS COMPARED -- miss=%u hitOnSky=%u. "
			            "Nothing was tested. Usual causes: the occupancy volume is unbuilt or "
			            "empty here (check the fluid perf line's occupancy= has stopped growing), "
			            "the camera is past the volume's ~25.6 m reach, or the quad path is "
			            "suppressed so there is no raster depth to compare against."),
			       G.Miss, G.HitOnSky);
		}
		else
		{
			// ---- THE VERDICT, AND WHAT CHANGED ABOUT IT ---------------------
			//
			// The old bar was "total disagreement < 0.5% AND interior == 0". Two
			// runs showed why that cannot work: 99.8% of disagreements were
			// silhouette pixels, where the two renderers are entitled to
			// disagree because they are point samples of a step function, and
			// the edge mask only credits 54-62% of them. A bar applied to that
			// total cannot pass however correct the marcher is, so a FAIL from
			// it carried no information.
			//
			// The verdict is now RELATIVE TO THE CONTROL and scoped to the
			// INTERIOR, which is the half with no excuse available. Edges are
			// reported beside their own control and are informational.
			const uint32 InteriorPx = (G.Compared > G.EdgePixels) ? (G.Compared - G.EdgePixels) : 0u;
			const double TotalRate = double(G.Disagree) / double(G.Compared);
			const double TotalCtl = double(G.RefSelfDisagree) / double(G.Compared);
			const double InnerRate = InteriorPx > 0 ? double(G.DisagreeInterior) / double(InteriorPx) : 0.0;
			const double InnerCtl = InteriorPx > 0 ? double(G.RefSelfDisagreeInterior) / double(InteriorPx) : 0.0;
			const double InnerMaxVox = double(G.InteriorMaxMilliVoxels) / 1000.0;

			// TWO CONDITIONS, AND BOTH ARE ABOUT THE INTERIOR.
			//   1. the interior disagreement rate is at or below the control's
			//   2. no interior pixel is off by a whole voxel
			// The second is not redundant: a rate can sit under the control
			// while a handful of pixels are wrong by metres, and that is a
			// different failure from sampling noise.
			const bool bWithinNoise = (InnerRate <= InnerCtl);
			const bool bNoLargeInterior = (InnerMaxVox < 1.0);
			const bool bPass = bWithinNoise && bNoLargeInterior;

			// THE WORDING IS THE SPIKE'S, DELIBERATELY. The control is an upper
			// bound on the noise floor, so being under it cannot certify -- it
			// can only refuse to condemn. "WITHIN REFERENCE NOISE (not proof)"
			// is what that state is called in this project and it is what gets
			// recorded, rather than a bare PASS that would be over-claimed.
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("Voxel march depth gate [%u frames]: %s"),
			       G.SampleFrames,
			       bPass ? TEXT("WITHIN REFERENCE NOISE (not proof)")
			             : (bNoLargeInterior ? TEXT("ABOVE REFERENCE NOISE -- hard result")
			                                 : TEXT("FAIL -- interior pixel off by >= 1 voxel")));
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("  INTERIOR (the gate): px=%u disagree=%u (%.4f%%) vs control %u (%.4f%%) "
			            "| max=%.3f vox (bar 1.0) | ctlMax=%.3f vox | "
			            "<0.25=%u <0.5=%u <1=%u <2=%u <4=%u >=4=%u"),
			       InteriorPx, G.DisagreeInterior, InnerRate * 100.0,
			       G.RefSelfDisagreeInterior, InnerCtl * 100.0, InnerMaxVox,
			       double(G.RefSelfMaxMilliVoxels) / 1000.0,
			       G.InteriorBuckets[0], G.InteriorBuckets[1], G.InteriorBuckets[2],
			       G.InteriorBuckets[3], G.InteriorBuckets[4], G.InteriorBuckets[5]);
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("  EDGES (informational): compared=%u disagree=%u (%.4f%%) vs control %u "
			            "(%.4f%%) | edgeMaskCovers=%.2f%% | allPxMax=%.3f vox = the SCENE DEPTH "
			            "RANGE at a silhouette, not an error | miss=%u hitOnSky=%u"),
			       G.Compared, G.Disagree, TotalRate * 100.0, G.RefSelfDisagree, TotalCtl * 100.0,
			       100.0 * double(G.EdgePixels) / double(G.Compared),
			       double(G.MaxDeltaMilliVoxels) / 1000.0, G.Miss, G.HitOnSky);
			if (G.RefSelfDisagree == 0u && G.Disagree > 0u)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("  control scored ZERO while the marcher scored %u. A control that "
				            "never fires is not a denominator -- check that the raster depth is "
				            "real and that the quad path is drawing before trusting the verdict."),
				       G.Disagree);
			}
		}
	}));

namespace
{
	// Published from the game thread, read from the render thread. Plain atomics
	// rather than a lock: three integers written once per tick and read once per
	// frame, and a torn read would at worst delay a gate by one frame.
	std::atomic<int32> GStreamConvergedFrames{-1};
	std::atomic<int64> GStreamLastLoaded{-1};
}

void VoxelMarchPublishStreamingState(int32 JobsInFlight, int32 PendingJobs, int64 ChunksLoaded)
{
	// CONVERGED IS ALL THREE, HELD. jobsInFlight and pendingJobs both zero is a
	// lull if the chunk count is still climbing -- the count is what says the
	// world stopped changing rather than merely stopped being busy.
	const bool bIdle = (JobsInFlight == 0) && (PendingJobs == 0);
	const bool bCountStable = (ChunksLoaded == GStreamLastLoaded.load());
	GStreamLastLoaded.store(ChunksLoaded);
	if (bIdle && bCountStable)
	{
		const int32 Prev = GStreamConvergedFrames.load();
		GStreamConvergedFrames.store(Prev < 0 ? 1 : Prev + 1);
	}
	else
	{
		GStreamConvergedFrames.store(0);
	}
}

int32 VoxelMarchGetStreamConvergedFrames()
{
	return GStreamConvergedFrames.load();
}

bool VoxelMarchIsStreamConverged()
{
	const int32 Frames = GStreamConvergedFrames.load();
	if (Frames < 0)
	{
		// -1 is "the publisher has never been called". The wire is DEAD, and a
		// dead wire is not a settled world -- the same distinction the depth
		// gate makes at bStreamWired.
		return false;
	}
	return Frames >= FMath::Max(CVarVoxelMarchSettleFrames.GetValueOnAnyThread(), 0);
}

FVoxelMarchStats VoxelMarchGetStats()
{
	if (!GMarchState.IsValid())
	{
		// Never created. Frames == 0 is the "nobody asked for anything" answer
		// and it is not the same as "it ran and drew nothing".
		return FVoxelMarchStats();
	}
	return GMarchState->GetStats();
}

// ---- THE RAY-BOUND CENSUS'S WINDOW (Stage 0a) -----------------------------
//
// A file-static beside the state rather than fields on FVoxelMarchHoleStats,
// deliberately: Stage 0a's edit surface is this file and the two shader files
// (see VoxelMarchRayBoundWord below the permutation dims), and the census is
// expected to be promoted into the struct or deleted once its gate is read.
// Same accumulation discipline as HoleWindow -- event counts SUMMED across
// landed frames, guarded by GMarchState->Lock at both the landing and the
// drain -- and drained/reset by VoxelMarchGetAndResetHoleStats on the same
// 5 s cadence, where its log line prints beside the other hole lines.
struct FVoxelMarchRayBoundWindow
{
	uint64 ProbesUp = 0;    // chunk-loop iterations, DirWorld.z >= 0
	uint64 ProbesDown = 0;  // chunk-loop iterations, DirWorld.z < 0
	uint64 PreUp = 0;       // iterations before a walk's first resident chunk
	uint64 PreDown = 0;
	uint64 PostUp = 0;      // trailing iterations of hitless walks
	uint64 PostDown = 0;
	uint64 CapRays = 0;     // rays with any TERM_CHUNK_CAP walk
	// The bound arm's engagement group (voxel.March.Bound, Stage 0b) --
	// words 56-59, appended to the same buffer, landed by the same readback,
	// printed on this census line's SIBLING (the bound engagement line in
	// the drain below). Zero on every frame whose kernel was not the bound
	// permutation, which the sibling line words as such rather than as "cut
	// nothing".
	uint64 BoundConsulted = 0;
	uint64 BoundSegmentsSkipped = 0;
	uint64 BoundWalkInRaised = 0;
	uint64 BoundWalkOutLowered = 0;
	uint64 Frames = 0;      // frames whose readback landed into this window
};
static FVoxelMarchRayBoundWindow GVoxelMarchRayBoundWindow;

FVoxelMarchHoleStats VoxelMarchGetAndResetHoleStats()
{
	FVoxelMarchHoleStats Out;
	// bArmed comes from the cvar, not from whether anything landed, so the
	// perf line can distinguish "on but no readback yet" (a warning-shaped
	// zero) from "off" (silence). The distinction is the whole reason the
	// shadow census has a refusal path.
	const FVoxelMarchArm Arm = VoxelMarchGetArm();
	Out.bArmed = Arm.bHoleStats;
	Out.bBreakdownArmed = Arm.HoleStatsLevel >= 2;
	// From the CVAR, like the two above, and NOT from whether any zcut word is
	// non-zero: "the arm is off" and "the arm is on and cut nothing" are
	// different findings and a consumer must be able to tell them apart without
	// inspecting the numbers it is trying to interpret.
	Out.bZCutArmed = CVarVoxelMarchZCut.GetValueOnAnyThread() != 0;
	// FROM THE CVAR, like the Z bound's flag and for the same reason: this arm
	// is a UNIFORM, so the cvar IS the arm. bHeightVerifyArmed is carried
	// separately because heightMissed == 0 is meaningless without it -- an
	// unarmed falsifier cannot produce a violation, and a reader must not be
	// able to mistake that for a falsifier that passed.
	Out.bHeightArmed = CVarVoxelMarchHeightPyramid.GetValueOnAnyThread() != 0;
	Out.bHeightVerifyArmed = CVarVoxelMarchHeightPyramidVerify.GetValueOnAnyThread() != 0;
	// THE FIELD'S OWN STATE, stamped beside the counters rather than left to be
	// inferred from them. A field that is entirely +INF is INERT: every air test
	// is false, the walk is exactly the control, and the counters below are all
	// structurally zero. Without these numbers that reads as a clean null result
	// -- which is precisely the misreading this project has published before.
	{
		const FVoxelHeightPyramid::FCensus HC = GetGlobalVoxelHeightPyramid().GetCensus();
		const FVoxelHeightPyramid::FGeometry HG = GetGlobalVoxelHeightPyramid().GetGeometry();
		Out.HeightLeafChunkLevel = HG.LeafChunkLevel;
		Out.HeightDim = HG.Dim;
		Out.HeightFilledLeaves = HC.Filled;
		Out.HeightInfiniteLeaves = HC.Infinite;
		Out.HeightDeclinedLeaves = HC.Declined;
		Out.HeightNotResidentLeaves = HC.NotResident;
		Out.HeightEditedLeaves = HC.Edited;
		Out.HeightMinUU = HC.MinUU;
		Out.HeightMaxUU = HC.MaxUU;
	}
	// FROM THE ARM, NOT FROM THE CVAR, and that is not a shortcut -- it is the
	// difference between the two switches. voxel.March.ZCut is a uniform, so the
	// cvar IS the arm. voxel.March.BlockSkip selects a PERMUTATION, and the arm
	// forces it off without rings; reading the raw cvar here would report
	// "armed" for a kernel that does not contain the code, and the inert-arm
	// warning below would then fire on a leg where inertness is correct.
	Out.bBlockSkipArmed = Arm.bBlockSkip;
	// FROM THE ARM, for the reason bBlockSkipArmed is: VoxelMarchGetArm already
	// forces the mode to 0 when the permutation is off, so this can never report
	// a licence the kernel has no code for.
	Out.BlockSkyMode = Arm.BlockSkyMode;
	// The chunk-cell census is compiled under VOXEL_MARCH_HOLE_STATS at ANY
	// level, so its arming is the hole-stats arming and nothing else. Carried
	// as its own named flag rather than left to the reader to infer from
	// bArmed: the eight census words can be zero for two entirely different
	// reasons (never compiled in, or compiled in against a kernel that made no
	// lookups) and a consumer must be able to separate them without inspecting
	// the numbers it is trying to interpret -- the same rule bZCutArmed and
	// bBlockSkipArmed exist for.
	Out.bCensusArmed = Arm.bHoleStats;
	if (!GMarchState.IsValid())
	{
		return Out;
	}
	FScopeLock Guard(&GMarchState->Lock);
	const bool bArmed = Out.bArmed;
	const bool bBreakdownArmed = Out.bBreakdownArmed;
	const bool bZCutArmed = Out.bZCutArmed;
	const bool bBlockSkipArmed = Out.bBlockSkipArmed;
	const bool bCensusArmed = Out.bCensusArmed;
	const int32 BlockSkyMode = Out.BlockSkyMode;
	// The height field's flags and census are NOT part of the accumulated
	// window -- they describe the field as it stands now, not what the GPU
	// counted -- so they are saved and restored across the assignment exactly
	// as the arm flags are.
	const FVoxelMarchHoleStats HeightSide = Out;
	Out = GMarchState->HoleWindow;
	Out.bArmed = bArmed;
	Out.bBreakdownArmed = bBreakdownArmed;
	Out.bZCutArmed = bZCutArmed;
	Out.bBlockSkipArmed = bBlockSkipArmed;
	Out.bCensusArmed = bCensusArmed;
	Out.BlockSkyMode = BlockSkyMode;
	Out.bHeightArmed = HeightSide.bHeightArmed;
	Out.bHeightVerifyArmed = HeightSide.bHeightVerifyArmed;
	Out.HeightLeafChunkLevel = HeightSide.HeightLeafChunkLevel;
	Out.HeightDim = HeightSide.HeightDim;
	Out.HeightFilledLeaves = HeightSide.HeightFilledLeaves;
	Out.HeightInfiniteLeaves = HeightSide.HeightInfiniteLeaves;
	Out.HeightDeclinedLeaves = HeightSide.HeightDeclinedLeaves;
	Out.HeightNotResidentLeaves = HeightSide.HeightNotResidentLeaves;
	Out.HeightEditedLeaves = HeightSide.HeightEditedLeaves;
	Out.HeightMinUU = HeightSide.HeightMinUU;
	Out.HeightMaxUU = HeightSide.HeightMaxUU;
	// Kept for the HUD's 1 Hz peek -- the panel must show what the log drained
	// without becoming a second drainer (two drainers of one accumulator each
	// see a random share).
	GMarchState->LastDrainedHoleWindow = Out;
	GMarchState->HoleWindow = FVoxelMarchHoleStats();

	// ---- THE RAY-BOUND CENSUS'S DRAIN AND LINE (Stage 0a) ------------------
	//
	// Drained here, on the same call the perf line makes every 5 s, so the
	// window it prints is exactly the window the other hole lines describe.
	// Logged from THIS file rather than woven into VoxelWorldSubsystem's perf
	// block because the census's whole plumbing is Stage 0a's three-file edit
	// surface; the line lands adjacent in the log either way.
	//
	// With the arm OFF this prints NOTHING, deliberately: the subsystem's cell
	// census line already prints NOT MEASURED for the whole hole-stats family
	// each window, and an absent line cannot be misread as a healthy zero the
	// way a printed 0 can.
	//
	// "probe" IN THIS LINE MEANS A CHUNK-LOOP ITERATION, NOT A LOOKUP -- the
	// cell census's cellsProbed counts VoxelMarchLookupChunk calls and the two
	// units differ (the loop caches the lookup within a chunk), so never read
	// this line's numbers against that one's.
	{
		const FVoxelMarchRayBoundWindow Rb = GVoxelMarchRayBoundWindow;
		GVoxelMarchRayBoundWindow = FVoxelMarchRayBoundWindow();
		if (Out.bArmed)
		{
			const uint64 Probes = Rb.ProbesUp + Rb.ProbesDown;
			if (Rb.Frames == 0)
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("Voxel march ray-bound census: armed, no readback landed "
				            "this window. No-sample, not a healthy zero."));
			}
			else if (Probes == 0 && Out.Rays != 0)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("Voxel march ray-bound census: ARMED AND INERT -- 0 chunk-loop "
				            "iterations over %llu measured frames in which the marcher "
				            "walked %llu rays. A ray that walks cannot loop zero times, so "
				            "this is the INSTRUMENT and not the world: grep "
				            "VOXEL_MARCH_HOLE_RB_PROBES in VoxelMarch.usf and "
				            "GVoxelMarchRayChunkIters in VoxelBrickTraverse.ush before "
				            "looking anywhere else."),
				       (unsigned long long)Rb.Frames, (unsigned long long)Out.Rays);
			}
			else
			{
				// removable = pre + post, per direction; the GO gate is
				// removable >= 40% of that direction's probes at the horizon,
				// or capRays >= 5% of rays. Rates are per direction so the
				// horizon clause reads straight off removableUp.
				const double Up = double(Rb.ProbesUp);
				const double Down = double(Rb.ProbesDown);
				const double Rays = double(Out.Rays);
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("Voxel march ray-bound census (probe = chunk-loop iteration, "
				            "NOT lookup): probes=%llu over %llu frames | UP/HORIZON "
				            "probesUpTotal=%llu probesPre=%llu (%.2f%% of up) "
				            "probesPost=%llu (%.2f%% of up) removableUp=%.2f%% | DOWN "
				            "probesDownTotal=%llu probesPreDown=%llu (%.2f%% of down) "
				            "probesPostDown=%llu (%.2f%% of down) removableDown=%.2f%% | "
				            "capRays=%llu (%.4f%% of %llu rays)"),
				       (unsigned long long)Probes, (unsigned long long)Rb.Frames,
				       (unsigned long long)Rb.ProbesUp,
				       (unsigned long long)Rb.PreUp,
				       Up > 0.0 ? 100.0 * double(Rb.PreUp) / Up : 0.0,
				       (unsigned long long)Rb.PostUp,
				       Up > 0.0 ? 100.0 * double(Rb.PostUp) / Up : 0.0,
				       Up > 0.0 ? 100.0 * double(Rb.PreUp + Rb.PostUp) / Up : 0.0,
				       (unsigned long long)Rb.ProbesDown,
				       (unsigned long long)Rb.PreDown,
				       Down > 0.0 ? 100.0 * double(Rb.PreDown) / Down : 0.0,
				       (unsigned long long)Rb.PostDown,
				       Down > 0.0 ? 100.0 * double(Rb.PostDown) / Down : 0.0,
				       Down > 0.0 ? 100.0 * double(Rb.PreDown + Rb.PostDown) / Down : 0.0,
				       (unsigned long long)Rb.CapRays,
				       Rays > 0.0 ? 100.0 * double(Rb.CapRays) / Rays : 0.0,
				       (unsigned long long)Out.Rays);
			}
		}

		// ---- THE BOUND ENGAGEMENT LINE (voxel.March.Bound, Stage 0b) -----
		//
		// THE CENSUS LINE'S SIBLING: same drain, same window, same buffer,
		// words 56-59. Gated on the RAW CVAR rather than the arm so that a
		// forced-off frame (sky ladder holding the permutation, or rings
		// off) prints WHY instead of printing nothing -- an absent line
		// there would read as "the leg forgot the cvar", which is a
		// different finding from "the arm yielded".
		if (CVarVoxelMarchBound.GetValueOnAnyThread() != 0)
		{
			const FVoxelMarchArm BndArm = VoxelMarchGetArm();
			const float BoundMs = GMarchState->Stats.BoundGpuMs;
			if (BndArm.Bound == 0)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("Voxel march bound: voxel.March.Bound is 1 but the arm is FORCED "
				            "OFF (%s). Every frame in this window ran the CONTROL -- do not "
				            "read it as a bound result."),
				       BndArm.bSkyLadder
				           ? TEXT("voxel.March.SkyLadder holds the refused pairing and wins")
				           : TEXT("rings are off, and the clamp lives in the ring walk"));
			}
			else if (!Out.bArmed)
			{
				// The engagement words ride the hole-stats permutation; a
				// bound leg without it has a working clamp and a mute
				// instrument, which must never print as zeros.
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("Voxel march bound: armed; engagement NOT MEASURED (the counters "
				            "ride voxel.March.HoleStats, which is 0). boundMs=%.3f. Turn "
				            "HoleStats on before believing any timing from this arm."),
				       BoundMs);
			}
			else if (Rb.Frames == 0)
			{
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("Voxel march bound: armed, no readback landed this window. "
				            "No-sample, not a healthy zero."));
			}
			else if (Rb.BoundConsulted == 0 && Out.Rays != 0)
			{
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("Voxel march bound: ARMED AND INERT -- 0 consults over %llu "
				            "measured frames in which the marcher walked %llu rays. The "
				            "producer declined (pool unflushed?), the texture went unbound, "
				            "or the loader never ran: check for the 'FORCED OFF' line above, "
				            "then grep VoxelMarchBoundProduce's decline paths and "
				            "GVoxelMarchBoundLoaded before looking anywhere else. No timing "
				            "taken on this leg means anything."),
				       (unsigned long long)Rb.Frames, (unsigned long long)Out.Rays);
			}
			else
			{
				// consulted partitions with skipped against the two clip
				// counters only loosely (a consult can raise AND lower), so
				// the line prints all four and the untouched remainder is
				// consulted - skipped - max(raised, lowered) at worst --
				// left to the reader rather than mis-summed here. DECISIONS,
				// NOT NANOSECONDS: the saving is VoxelMarch.March from
				// ProfileGPU net of boundMs, and nothing else.
				const double C = double(Rb.BoundConsulted);
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("Voxel march bound engagement: consulted=%llu over %llu frames | "
				            "segmentsSkipped=%llu (%.2f%%) walkInRaised=%llu (%.2f%%) "
				            "walkOutLowered=%llu (%.2f%%) | producer boundMs=%.3f (its own "
				            "bracket, NOT inside marchMs). Skipped is the PRIMARY counter -- "
				            "the census put the removable mass in pre-hit ring segments."),
				       (unsigned long long)Rb.BoundConsulted, (unsigned long long)Rb.Frames,
				       (unsigned long long)Rb.BoundSegmentsSkipped,
				       C > 0.0 ? 100.0 * double(Rb.BoundSegmentsSkipped) / C : 0.0,
				       (unsigned long long)Rb.BoundWalkInRaised,
				       C > 0.0 ? 100.0 * double(Rb.BoundWalkInRaised) / C : 0.0,
				       (unsigned long long)Rb.BoundWalkOutLowered,
				       C > 0.0 ? 100.0 * double(Rb.BoundWalkOutLowered) / C : 0.0,
				       BoundMs);
			}

			// ---- THE CULL'S OWN LINE (2026-08-28, the boundMs mitigation) --
			//
			// The frustum cull is the change that has to pay boundMs down
			// from 5.520, and an arm that cannot show it fired is not an
			// arm: this line proves engagement (culled > 0) and says what
			// fraction of the pool the draw no longer pays for. The
			// identity considered == culled + sum(drawn) is exact because
			// all three numbers ride one dispatch's two buffers, read back
			// side by side into one slot; a FAIL indicts the INSTRUMENT (a
			// return path in the list pass not counted) and the fractions
			// must not be read on such a window.
			const FVoxelMarchBoundCullStats Cull = VoxelMarchBoundGetAndResetCullStats();
			if (BndArm.Bound != 0 && Cull.Frames > 0)
			{
				uint64 DrawnTotal = 0;
				for (int32 L = 0; L < 7; ++L)
				{
					DrawnTotal += Cull.DrawnPerLevel[L];
				}
				const bool bIdentityOk = (Cull.Considered == Cull.Culled + DrawnTotal);
				if (Cull.LastEnable == 0)
				{
					// Loud, because culled == 0 with the cull DISABLED must
					// never read as "nothing lay outside the frustum" --
					// disabled means the CPU refused a degenerate frustum
					// and every cube was drawn, i.e. BND-eng2's cost.
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("Voxel march bound cull: DISABLED (degenerate frustum on "
					            "the CPU -- fail-open, every cube drawn). boundMs on this "
					            "window is the UNCULLED cost; fix the view matrices "
					            "before reading it as the mitigation's result."));
				}
				const double Cons = double(Cull.Considered);
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("Voxel march bound cull: considered=%llu culled=%llu (%.2f%%) "
				            "drawn=%llu (L0..L6: %llu/%llu/%llu/%llu/%llu/%llu/%llu) over "
				            "%llu frames | identity considered==culled+drawn: %s"),
				       (unsigned long long)Cull.Considered, (unsigned long long)Cull.Culled,
				       Cons > 0.0 ? 100.0 * double(Cull.Culled) / Cons : 0.0,
				       (unsigned long long)DrawnTotal,
				       (unsigned long long)Cull.DrawnPerLevel[0],
				       (unsigned long long)Cull.DrawnPerLevel[1],
				       (unsigned long long)Cull.DrawnPerLevel[2],
				       (unsigned long long)Cull.DrawnPerLevel[3],
				       (unsigned long long)Cull.DrawnPerLevel[4],
				       (unsigned long long)Cull.DrawnPerLevel[5],
				       (unsigned long long)Cull.DrawnPerLevel[6],
				       (unsigned long long)Cull.Frames,
				       bIdentityOk ? TEXT("PASS")
				                   : TEXT("FAIL -- a list-pass return path is uncounted; "
				                          "do not read the fractions on this window"));
			}
		}
	}
	return Out;
}

FVoxelMarchHoleStats VoxelMarchPeekLastHoleWindow()
{
	FVoxelMarchHoleStats Out;
	const FVoxelMarchArm Arm = VoxelMarchGetArm();
	Out.bArmed = Arm.bHoleStats;
	Out.bBreakdownArmed = Arm.HoleStatsLevel >= 2;
	if (!GMarchState.IsValid())
	{
		// Frames == 0: "no window has completed", which the panel must word as
		// no-sample, never as a healthy zero.
		return Out;
	}
	FScopeLock Guard(&GMarchState->Lock);
	const bool bArmed = Out.bArmed;
	const bool bBreakdownArmed = Out.bBreakdownArmed;
	const bool bZCutArmed = CVarVoxelMarchZCut.GetValueOnAnyThread() != 0;
	// See the drain: the arm, not the cvar, because this one is a permutation.
	const bool bBlockSkipArmed = Arm.bBlockSkip;
	// See the drain: the census rides the hole-stats permutation itself.
	const bool bCensusArmed = Arm.bHoleStats;
	const int32 BlockSkyMode = Arm.BlockSkyMode;
	Out = GMarchState->LastDrainedHoleWindow;
	// The arm flags track the switch NOW, not the switch as it stood when the
	// stale window was drained -- the panel's off/armed wording follows the
	// cvar the owner just typed. Same rule for the height arm.
	Out.bHeightArmed = CVarVoxelMarchHeightPyramid.GetValueOnAnyThread() != 0;
	Out.bHeightVerifyArmed = CVarVoxelMarchHeightPyramidVerify.GetValueOnAnyThread() != 0;
	Out.bArmed = bArmed;
	Out.bBreakdownArmed = bBreakdownArmed;
	Out.bZCutArmed = bZCutArmed;
	Out.bBlockSkipArmed = bBlockSkipArmed;
	Out.bCensusArmed = bCensusArmed;
	Out.BlockSkyMode = BlockSkyMode;
	return Out;
}

// ===========================================================================
// Shader parameter structs
// ===========================================================================

// Everything the ray setup needs, shared by the march kernel and (for the
// velocity reconstruction) the emit. ONE struct, pasted into both, so the two
// passes provably build the same ray for the same pixel -- the same guarantee
// VoxelMarchSpike.usf's census makes about tracing "the same rays, not merely
// similar ones".
BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchViewParameters, )
	SHADER_PARAMETER(FMatrix44f, MarchViewToTranslatedWorld)
	SHADER_PARAMETER(FVector3f, MarchRayOriginLocalUU)
	SHADER_PARAMETER(float, MarchVolumeExtentUU)
	SHADER_PARAMETER(FVector2f, MarchViewRectMin)
	SHADER_PARAMETER(FVector2f, MarchViewRectSize)
	SHADER_PARAMETER(FVector2f, MarchInvProjDiag)
	// THE ENGINE'S TAA/TSR JITTER FOR THIS FRAME, IN NDC (voxel.March.TAAJitter),
	// or (0,0) when the arm is off. Subtracted from NDC before the ray is built,
	// which is the exact inverse of what UE does to the projection's third row.
	//
	// RIDES THIS STRUCT FOR THE REASON THE WHOLE STRUCT EXISTS: the march and
	// the emit's reconstruction must build the SAME ray for the same pixel, and
	// a jitter applied to one and not the other is a sub-voxel depth error on
	// every pixel. Bound on every permutation -- a shader global with no entry
	// here is an UNBOUND uniform, which fails the global shader compile outright.
	SHADER_PARAMETER(FVector2f, MarchTemporalAAJitter)
	// THE HALF-RES LATTICE'S PER-FRAME OFFSET, in full-res pixels, inside the
	// 2x2 block. It rides THIS struct rather than any one pass's own parameters
	// precisely because the march, the depth pre-emit, the GBuffer emit and the
	// verify gate must all place the sample at the same point -- the same reason
	// MarchInvProjDiag is here. Zero at full res and under Jitter 0, and bound
	// on every permutation: a shader global with no entry here is an UNBOUND
	// uniform, which fails the global shader compile outright.
	SHADER_PARAMETER(FVector2f, MarchSampleJitter)
	SHADER_PARAMETER(FVector4f, MarchInvDeviceZToWorldZ)
	SHADER_PARAMETER(float, MarchPixelConeSlope)
	SHADER_PARAMETER(float, MarchClimateStrength)
	// NOTE: the volume's origin in TRANSLATED world is deliberately NOT here.
	// The emit derives it as TranslatedWorldCameraOrigin - MarchRayOriginLocalUU,
	// which is exact; passing it as a uniform would mean assuming
	// PreViewTranslation == -ViewOrigin, which holds for the primary view and is
	// not a thing to build on.
	SHADER_PARAMETER(FIntPoint, MarchTileCount)
END_SHADER_PARAMETER_STRUCT()

// BOTH SOURCES' BINDINGS RIDE ONE STRUCT, and both are filled on every dispatch
// regardless of which permutation runs. The alternative -- a struct per source --
// would need a shader class per source, and the two would then have separate
// parameter plumbing to drift apart. Filling both costs four SRV assignments on
// a pass that marches 1.3M rays.
BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchCSParameters, )
	VOXEL_FLUID_OCCUPANCY_PARAMETERS()
	VOXEL_BRICK_POOL_PARAMETERS()
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchChunkIndex)
	// THE COARSE OCCUPANCY LEVEL (voxel.March.BlockSkip). One bit per 4x4x4
	// block of chunk cells: Occupied means at least one cell in the block is
	// resident, AnyAbsent means at least one is not. 32 KiB each against the
	// 64 MiB they bound.
	//
	// BOUND ON EVERY PERMUTATION, INCLUDING VOXEL_MARCH_BLOCK_SKIP 0, for the
	// reason the cover pair below is bound on every arm -- and here the stakes
	// are higher than a silent zero. An unbound Buffer<uint> reads as ZEROS,
	// zero in Occupied means "no chunk in this block is resident", and a
	// marcher that believed that would skip the entire world in one jump: every
	// ray a hole, no error anywhere. FVoxelMarchChunkIndex::RegisterWithBlocks
	// therefore guarantees a non-null pair whenever it returns an index at all,
	// falling back to an ALL-ONES grid (descend everywhere, i.e. the control)
	// rather than to nothing.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockOccupied)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockAnyAbsent)
	// THE SKY LICENCE (voxel.March.BlockSkySkip). One bit per block: all 64
	// cells carry a valid open-sky mark AND none is resident.
	//
	// BOUND ON EVERY PERMUTATION for the reason the pair above is, but its
	// FAILURE DIRECTION IS THE OPPOSITE and that is worth saying at the binding
	// rather than only at the writer. Unbound reads as zeros; zero here means
	// "no block is provably sky", so the arm licenses nothing and the marcher
	// descends exactly as the control does. INERT, not wrong -- which is why
	// this one falls back to all ZEROS while the occupancy pair falls back to
	// all ones.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockAllSky)
	// 0 residency-only / 1 both / 2 sky-only. A uniform rather than a
	// permutation -- see FVoxelMarchArm::BlockSkyMode for that argument. Bound
	// on every permutation because an unset uniform is a silent zero, and zero
	// is also the correct "sky licence off" value: the safe default and the
	// honest default are the same number here.
	SHADER_PARAMETER(uint32, MarchBlockSkyMode)
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
	SHADER_PARAMETER(uint32, MarchIndexCellsPerLevel)
	// PHASE 6. Bound on every permutation, including the ones compiled with
	// VOXEL_MARCH_COVER 0: a shader-side global with no entry in the struct is
	// an unbound parameter, and reach 0 is the honest "cover is off" value.
	SHADER_PARAMETER(uint32, MarchCoverIndexGrid)
	SHADER_PARAMETER(float, MarchCoverReachUU)
	// One bit per index grid slot: set means that slot holds at least one
	// resident chunk. Read ONLY by the voxel.March.HoleStats 2 permutation
	// (VoxelMarchAbsentTouchesShell), bound on every arm for the reason the
	// cover pair above is -- an unbound uniform is a silent zero, and a zero
	// here means every slot is empty, which would make every absent chunk read
	// as a hole rather than none.
	SHADER_PARAMETER(uint32, MarchIndexLevelPopulated)
	// THE RESIDENT-Z BOUND (voxel.March.ZCut). Bound on every permutation for
	// the reason the two above are: an unbound uniform is a silent zero, and
	// zero in MarchZCutEnable is the honest "do not cut" value, so the safe
	// default and the correct default are again the same number.
	//
	// One int4 per INDEX GRID SLOT, not per ring level -- the cover slot has
	// its own walk and is simply never given a usable bound.
	//   .x  min chunk Z, already padded outward
	//   .y  max chunk Z, already padded outward, INCLUSIVE
	//   .z  1 = this slot's bound is usable; 0 = DO NOT CUT this slot
	//   .w  unused, 0
	// Padding is applied CPU-side so the shader has one number to trust and
	// there is no second place for the pad to be forgotten.
	SHADER_PARAMETER(int32, MarchZCutEnable)
	SHADER_PARAMETER_ARRAY(FIntVector4, MarchLevelChunkZ, [8])
	// THE PER-RAY RESIDENT-EXTENT BOUND (voxel.March.Bound). The shader-side
	// globals exist ONLY under VOXEL_MARCH_BOUND (VoxelMarchBound.ush), so on
	// every other permutation these two entries are simply unused -- the
	// MarchOutHoleStats rule, not the MarchCoverReachUU one. The texture is
	// left NULL whenever the producer did not run this frame, and the
	// permutation is then NOT selected either: the two are decided from the
	// same local (BoundTex != nullptr) at the dispatch, so an armed cvar with
	// a declined producer can never hand the bound kernel an unbound texture
	// -- whose zero reads would decode as EMPTY intervals, i.e. skip the
	// world, the one failure direction this arm may never have.
	SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float2>, MarchBoundTex)
	// How many slices the producer rendered; the loader passes levels at or
	// past it through unclamped rather than loading out of array bounds
	// (which returns zeros -- see above for what zeros decode as).
	SHADER_PARAMETER(uint32, MarchBoundSliceCount)
	// ---- THE TERRAIN HEIGHT PYRAMID (voxel.March.HeightPyramid) ----------
	//
	// ON THIS STRUCT ONLY, AND THAT IS THE SAFETY ARGUMENT RATHER THAN A
	// CONVENIENCE. The globals these fill are declared in VoxelMarch.usf and
	// read only by VoxelMarchMain, so the emit and the two verify kernels --
	// which are bound through the same VoxelMarchBindPool template and would
	// otherwise all need an entry -- never see them. Declaring them in
	// VoxelBrickTraverse.ush instead would put a Buffer<float> global into four
	// parameter structs, and an unbound typed buffer reads as ZEROS. Zero at
	// this datum is SEA LEVEL, i.e. "the terrain tops out at the waterline",
	// which over the leg spawn's 2.8 km massif is proven air over real ground.
	// The failure would be silent, and it would look like a speedup.
	//
	// The buffer is NEVER null: when the field has not been built the binding
	// falls back to a ONE-FLOAT buffer holding +INF, so even a wrongly-enabled
	// arm reads "the terrain might reach arbitrarily high here" and marches
	// exactly the control. The safe fallback and the honest fallback are the
	// same value, which is the property that made +INF the representation.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, MarchHeightPyramid)
	// 0 = do not consult. Bound on every permutation of this struct; zero is
	// both the safe value and the honest "arm off" value.
	SHADER_PARAMETER(int32, MarchHeightEnable)
	SHADER_PARAMETER(int32, MarchHeightDim)          // leaf cells per side, power of two
	SHADER_PARAMETER(int32, MarchHeightNumMips)
	// log2 of the LEVEL-0 VOXELS per leaf cell edge. The DDA derives its cell
	// boundaries from world voxel indices and an arithmetic shift, exactly as
	// VoxelMarchCellExitT does -- see the note there about the local-frame
	// floor() that assumed a cell-aligned frame origin and silently tested the
	// wrong brick for 11.5% of rays.
	SHADER_PARAMETER(int32, MarchHeightLeafVoxelShift)
	// Leaf-cell coordinate of the field's minimum corner. THE RANGE TEST
	// AGAINST THIS IS WHAT MAKES THE INDEX SAFE: there is no toroidal wrap, so
	// a cell outside the field is not representable as an in-field index and
	// reads +INF instead of aliasing onto ground 13 km away.
	SHADER_PARAMETER(FIntPoint, MarchHeightMinCell)
	// Element offset of each mip inside the flat buffer, 8 ints as two int4s.
	SHADER_PARAMETER_ARRAY(FIntVector4, MarchHeightMipOffset, [2])
	// voxel.March.HeightPyramid.BiasM, converted to UU and clamped at >= 0.
	// SUBTRACTED from every bound the shader reads, to break the field on
	// purpose so the falsifier can be shown able to fire.
	SHADER_PARAMETER(float, MarchHeightBiasUU)
	SHADER_PARAMETER(int32, MarchHeightMaxIters)
	SHADER_PARAMETER(int32, MarchHeightVerify)
	// The march frame origin in level-0 world voxels, carried as this feature's
	// own uniform. Filled from MarchBrickOriginVoxel one line apart at the
	// dispatch, so there is no second authority -- see VoxelHeightPyramid.ush
	// for why the height walk cannot simply name that global.
	SHADER_PARAMETER(FIntVector, MarchHeightOriginVoxel)
	SHADER_PARAMETER(uint32, MarchRingCount)
	SHADER_PARAMETER(float, MarchRing0OuterUU)
	SHADER_PARAMETER(FIntVector, MarchPackOriginVoxel)
	SHADER_PARAMETER(FIntVector, MarchBrickOriginVoxel)
	SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelMarchViewParameters, MarchView)
	SHADER_PARAMETER(int32, MarchStepBudget)
	SHADER_PARAMETER(int32, MarchHasPrepassDepth)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, MarchSceneDepthTexture)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint2>, MarchOutVis)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, MarchOutHitT)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchOutTileHit)
	// The hole metric's stats words (voxel.March.HoleStats). Null -- not a
	// dummy buffer -- on every frame the arm is off: the off permutation has
	// no shader-side global, so the entry is simply unused, which is the
	// inverse of the MarchCoverReachUU note above and equally legal.
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchOutHoleStats)
END_SHADER_PARAMETER_STRUCT()

// ===========================================================================
// THE TERRAIN HEIGHT PYRAMID: FILLING THE BINDING
// ===========================================================================
//
// ONE PLACE, AND IT ALWAYS WRITES EVERY FIELD. An unset uniform is a silent
// zero and this file has paid for that more than twice; here a silent zero in
// MarchHeightDim would make the range test reject every cell, which is inert
// rather than wrong -- but a silent zero in the BUFFER would be sea level, so
// the buffer is never left unbound on any path out of this function.
static void VoxelMarchFillHeightPyramid(FRDGBuilder& GraphBuilder,
                                        FVoxelMarchCSParameters& Params)
{
	const bool bAsked = CVarVoxelMarchHeightPyramid.GetValueOnRenderThread() != 0;

	FVoxelHeightPyramid& Pyramid = GetGlobalVoxelHeightPyramid();
	const FVoxelHeightPyramid::FBinding B = Pyramid.BindForRender(GraphBuilder);

	// NEGATIVE IS CLAMPED AWAY. A negative bias is a MARGIN, and the crown
	// composition this bound already performs per layer is strictly tighter
	// than any global pad -- 89.2% of footprints get the 25 m L1 cap rather
	// than the 45 m global maximum. A margin here would be looser and would
	// also hide exactly the error the falsifier is looking for.
	const float BiasUU =
		FMath::Max(CVarVoxelMarchHeightPyramidBiasM.GetValueOnRenderThread(), 0.0f) * 100.0f;

	Params.MarchHeightBiasUU = BiasUU;
	// AT LEAST 1. Zero would mean "split the ray into no intervals", which the
	// shader would read as "walk nothing" -- a hole from a knob.
	Params.MarchHeightMaxIters =
		FMath::Clamp(CVarVoxelMarchHeightPyramidMaxIters.GetValueOnRenderThread(), 1, 8);
	Params.MarchHeightVerify =
		CVarVoxelMarchHeightPyramidVerify.GetValueOnRenderThread() != 0 ? 1 : 0;

	if (!B.bValid || B.Buffer == nullptr)
	{
		// THE FALLBACK IS ONE FLOAT HOLDING +INF, NOT A NULL SRV AND NOT A
		// ZERO-FILLED BUFFER. RDG requires the declared SRV to be bound, and
		// the two obvious ways to satisfy that are both wrong in the same
		// direction: a null SRV reads zeros and a cleared buffer IS zeros, and
		// zero at this datum is sea level. +INF makes every air test false, so
		// a wrongly-enabled arm over an unbuilt field marches EXACTLY the
		// control instead of deleting the world.
		const float Inf = VoxelHeightPyramidPositiveInfinity();
		FRDGBufferRef Absent = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), 1),
			TEXT("VoxelMarch.HeightPyramid.Absent"));
		GraphBuilder.QueueBufferUpload(Absent, &Inf, sizeof(float), ERDGInitialDataFlags::None);
		Params.MarchHeightPyramid = GraphBuilder.CreateSRV(Absent, PF_R32_FLOAT);
		Params.MarchHeightEnable = 0;
		Params.MarchHeightDim = 0;
		Params.MarchHeightNumMips = 0;
		Params.MarchHeightLeafVoxelShift = 0;
		Params.MarchHeightMinCell = FIntPoint::ZeroValue;
		Params.MarchHeightMipOffset[0] = FIntVector4(0, 0, 0, 0);
		Params.MarchHeightMipOffset[1] = FIntVector4(0, 0, 0, 0);
		GVoxelMarchHeightRanEnable.store(0, std::memory_order_relaxed);
		return;
	}

	Params.MarchHeightPyramid = GraphBuilder.CreateSRV(B.Buffer, PF_R32_FLOAT);
	Params.MarchHeightDim = B.Geom.Dim;
	Params.MarchHeightNumMips = B.Geom.NumMips;
	Params.MarchHeightLeafVoxelShift = B.Geom.LeafVoxelShift;
	Params.MarchHeightMinCell = FIntPoint(B.Geom.MinCellX, B.Geom.MinCellY);
	// The array is two int4s because the field carries at most kMaxMips = 8
	// levels. Spelled as [2] in the parameter struct and as int4[2] in
	// VoxelMarch.usf, so the one authority checks the two spellings HERE rather
	// than letting a deeper pyramid write past the uniform.
	static_assert(FVoxelHeightPyramid::kMaxMips == 8,
	              "MarchHeightMipOffset is declared [2] int4s here and in VoxelMarch.usf. "
	              "The pyramid grew a level; widen both, or the far mips read another "
	              "mip's offset and the DDA indexes the wrong level's cells.");
	// Named components rather than V[K]. FIntVector4's subscript is not the
	// spelling anything else in this file uses, and an offset written into the
	// wrong lane would index another MIP's cells -- a plausible height read at
	// the wrong resolution, which is invisible in review and is exactly the
	// class of error this feature must not be able to make.
	auto MipOff = [&B](int32 M) -> int32
	{
		return (M >= 0 && M < B.Geom.NumMips) ? B.Geom.MipOffset[M] : 0;
	};
	Params.MarchHeightMipOffset[0] = FIntVector4(MipOff(0), MipOff(1), MipOff(2), MipOff(3));
	Params.MarchHeightMipOffset[1] = FIntVector4(MipOff(4), MipOff(5), MipOff(6), MipOff(7));
	Params.MarchHeightEnable = (bAsked && B.Geom.Dim > 0 && B.Geom.NumMips > 0) ? 1 : 0;

	// WHAT WAS ACTUALLY UPLOADED, stamped from the values just written rather
	// than re-read from the cvar, for the reason the Z bound stamps its own: a
	// cvar reads back whatever was typed, and that reading alone is what let
	// nine switches in this project sit armed and inert. This proves the
	// uniform was FILLED -- not that the pass ran, and not that the shader used
	// it. Only the engagement counters say that.
	GVoxelMarchHeightRanEnable.store(Params.MarchHeightEnable, std::memory_order_relaxed);
	GVoxelMarchHeightRanDim.store(B.Geom.Dim, std::memory_order_relaxed);
	GVoxelMarchHeightRanLeafLevel.store(B.Geom.LeafChunkLevel, std::memory_order_relaxed);
	GVoxelMarchHeightRanMips.store(B.Geom.NumMips, std::memory_order_relaxed);
}


BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchCompactParameters, )
	SHADER_PARAMETER(FIntPoint, MarchTileCount)
	SHADER_PARAMETER(uint32, MarchTileTotal)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchTileHit)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchOutTileList)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchOutDrawArgs)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchEmitParameters, )
	SHADER_PARAMETER_STRUCT_REF(FVoxelGIVolumeParameters, VoxelGIVol)
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	VOXEL_FLUID_OCCUPANCY_PARAMETERS()
	VOXEL_BRICK_POOL_PARAMETERS()
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchChunkIndex)
	// THE COARSE OCCUPANCY LEVEL (voxel.March.BlockSkip). One bit per 4x4x4
	// block of chunk cells: Occupied means at least one cell in the block is
	// resident, AnyAbsent means at least one is not. 32 KiB each against the
	// 64 MiB they bound.
	//
	// BOUND ON EVERY PERMUTATION, INCLUDING VOXEL_MARCH_BLOCK_SKIP 0, for the
	// reason the cover pair below is bound on every arm -- and here the stakes
	// are higher than a silent zero. An unbound Buffer<uint> reads as ZEROS,
	// zero in Occupied means "no chunk in this block is resident", and a
	// marcher that believed that would skip the entire world in one jump: every
	// ray a hole, no error anywhere. FVoxelMarchChunkIndex::RegisterWithBlocks
	// therefore guarantees a non-null pair whenever it returns an index at all,
	// falling back to an ALL-ONES grid (descend everywhere, i.e. the control)
	// rather than to nothing.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockOccupied)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockAnyAbsent)
	// THE SKY LICENCE (voxel.March.BlockSkySkip). One bit per block: all 64
	// cells carry a valid open-sky mark AND none is resident.
	//
	// BOUND ON EVERY PERMUTATION for the reason the pair above is, but its
	// FAILURE DIRECTION IS THE OPPOSITE and that is worth saying at the binding
	// rather than only at the writer. Unbound reads as zeros; zero here means
	// "no block is provably sky", so the arm licenses nothing and the marcher
	// descends exactly as the control does. INERT, not wrong -- which is why
	// this one falls back to all ZEROS while the occupancy pair falls back to
	// all ones.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockAllSky)
	// 0 residency-only / 1 both / 2 sky-only. A uniform rather than a
	// permutation -- see FVoxelMarchArm::BlockSkyMode for that argument. Bound
	// on every permutation because an unset uniform is a silent zero, and zero
	// is also the correct "sky licence off" value: the safe default and the
	// honest default are the same number here.
	SHADER_PARAMETER(uint32, MarchBlockSkyMode)
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
	SHADER_PARAMETER(uint32, MarchIndexCellsPerLevel)
	// PHASE 6. Bound on every permutation, including the ones compiled with
	// VOXEL_MARCH_COVER 0: a shader-side global with no entry in the struct is
	// an unbound parameter, and reach 0 is the honest "cover is off" value.
	SHADER_PARAMETER(uint32, MarchCoverIndexGrid)
	SHADER_PARAMETER(float, MarchCoverReachUU)
	// One bit per index grid slot: set means that slot holds at least one
	// resident chunk. Read ONLY by the voxel.March.HoleStats 2 permutation
	// (VoxelMarchAbsentTouchesShell), bound on every arm for the reason the
	// cover pair above is -- an unbound uniform is a silent zero, and a zero
	// here means every slot is empty, which would make every absent chunk read
	// as a hole rather than none.
	SHADER_PARAMETER(uint32, MarchIndexLevelPopulated)
	// THE RESIDENT-Z BOUND (voxel.March.ZCut). Bound on every permutation for
	// the reason the two above are: an unbound uniform is a silent zero, and
	// zero in MarchZCutEnable is the honest "do not cut" value, so the safe
	// default and the correct default are again the same number.
	//
	// One int4 per INDEX GRID SLOT, not per ring level -- the cover slot has
	// its own walk and is simply never given a usable bound.
	//   .x  min chunk Z, already padded outward
	//   .y  max chunk Z, already padded outward, INCLUSIVE
	//   .z  1 = this slot's bound is usable; 0 = DO NOT CUT this slot
	//   .w  unused, 0
	// Padding is applied CPU-side so the shader has one number to trust and
	// there is no second place for the pad to be forgotten.
	SHADER_PARAMETER(int32, MarchZCutEnable)
	SHADER_PARAMETER_ARRAY(FIntVector4, MarchLevelChunkZ, [8])
	SHADER_PARAMETER(uint32, MarchRingCount)
	SHADER_PARAMETER(float, MarchRing0OuterUU)
	SHADER_PARAMETER(FIntVector, MarchPackOriginVoxel)
	SHADER_PARAMETER(FIntVector, MarchBrickOriginVoxel)
	SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelMarchViewParameters, MarchView)
	SHADER_PARAMETER(int32, MarchStepBudget)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint2>, MarchVis)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, MarchHitT)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchTileList)
	RDG_BUFFER_ACCESS(MarchDrawArgs, ERHIAccess::IndirectArgs)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

// ===========================================================================
// Shaders
// ===========================================================================

// The traversal source (VoxelBrickTraverse.ush's VOXEL_MARCH_SOURCE). A
// permutation dimension with exactly one value today, declared with room for
// the second so that P2's swap is a value change here and an #if there and
// nothing else anywhere.
// TWO VALUES NOW: 0 occupancy, 1 brick pool. It was 1 (i.e. only 0) while the
// pool traversal did not exist, so that "the pool is wired up" could not be true
// by accident -- it had to be typed. It has now been typed.
class FVoxelMarchSourceDim : SHADER_PERMUTATION_INT("VOXEL_MARCH_SOURCE", 2);
// 0 flat / 1 brick skip / 2 chunk skip. Only meaningful on source 1; the
// source-0 permutations at skip > 0 are refused below rather than compiled and
// never selected, because an unreachable permutation in the shader map is a
// permutation nobody notices is wrong.
class FVoxelMarchSkipDim : SHADER_PERMUTATION_INT("VOXEL_MARCH_SKIP_LEVELS", 3);
// The ring cascade arm. A PERMUTATION for the same reason the source is: a
// runtime branch leaves both paths in the binary and silently re-bases whichever
// arm is being measured, so rings-off would stop being a control.
class FVoxelMarchRingsDim : SHADER_PERMUTATION_BOOL("VOXEL_MARCH_RINGS");
// Fine -> coarse fallthrough depth (Phase 1, the no-hole invariant): 0 off
// (the byte-identical control), 1 the intended arm, 2 the measurable step
// beyond it. A PERMUTATION and not a uniform for the reason rings are; only
// meaningful with rings on and source 1, and the other combinations are
// refused below rather than compiled and never selected.
class FVoxelMarchFallthroughDim : SHADER_PERMUTATION_INT("VOXEL_MARCH_FALLTHROUGH", 3);
// The hole metric (voxel.March.HoleStats). A PERMUTATION so that off is a
// byte-identical control -- no UAV global, no groupshared, no atomics in the
// binary -- same rule as rings. DELIBERATELY NOT A WALK-SHAPE DIMENSION
// (FVoxelMarchWalkShape stays at 6): everything under VOXEL_MARCH_HOLE_STATS
// is observation -- it cannot change any ray's bHit or THitUU, so the
// comparator grades the same picture with it on or off. The bookkeeping it
// shares with fallthrough (bResident/bCrossedAbsentChunk, via
// VOXEL_MARCH_TRACK_ABSENT) writes fields only the counters read. If that
// ever stops being true -- if a hole-stats branch gains the power to change a
// hit -- it must move into the walk shape and be classified, per the
// static_assert below.
//
// AN INT SINCE 2026-08-23, three values: 0 off (unchanged, still the
// byte-identical control), 1 the certified cheap counters (unchanged counting
// code; only the shared word-count define grew), 2 the uncovered breakdown --
// per-level and per-reason words, plus the per-ray capture registers in the
// walk. Still observation-only at every level, same rule, same static_assert.
class FVoxelMarchHoleStatsDim : SHADER_PERMUTATION_INT("VOXEL_MARCH_HOLE_STATS", 3);

// ---- THE RAY-BOUND CENSUS'S WORDS (Stage 0a) ------------------------------
//
// These three counters exist to gate the rasteriser-bound arm (docs/plan):
// removable = probesPre + probesPost; the GO gate is removable >= 40% of total
// chunk probes at the horizon, or capRays >= 5% of rays. The unit is
// CHUNK-LOOP ITERATIONS, not VoxelMarchLookupChunk calls -- the statics in
// VoxelBrickTraverse.ush say why, and the log line names its fields to match.
//
// APPENDED AFTER VoxelMarchHoleWord::Count, and IN THIS FILE rather than in
// the header enum, deliberately: Stage 0a's edit surface is the two shader
// files and this one (the census gates a decision and is expected to be
// promoted into the enum or deleted once the gate is read), and appending
// derived-from-Count indices keeps the one-authority discipline anyway --
// these values are pushed to the shader as defines below and read back at the
// same expressions, so there is no hand mirror on either side. If a word is
// ever added to VoxelMarchHoleWord itself these shift with Count on both
// sides at once, exactly like an append inside the enum, with the same rule:
// force a shader recompile before trusting any counter.
namespace VoxelMarchRayBoundWord
{
	enum
	{
		ProbesUp = VoxelMarchHoleWord::Count,  // chunk-loop iterations, DirWorld.z >= 0 (THE DENOMINATOR, up/horizon)
		ProbesDown,                            // chunk-loop iterations, DirWorld.z < 0
		PreUp,                                 // iterations before a walk's first resident chunk, up/horizon
		PreDown,                               //   ... down
		PostUp,                                // trailing iterations of hitless walks, up/horizon
		PostDown,                              //   ... down
		CapRays,                               // rays with any TERM_CHUNK_CAP walk (not split; its gate is a share of ALL rays)

		// ---- THE BOUND'S ENGAGEMENT GROUP (voxel.March.Bound, Stage 0b) --
		// APPENDED AFTER THE CENSUS'S OWN WORDS, exactly as the census's
		// comment above instructs ("extend the word list if you add
		// counters"): appending renumbers nothing, a stale shader cache
		// still reads every older word correctly and leaves these four at
		// zero -- which the reader has to word as NOT MEASURED anyway.
		// Written only by kernels compiled with VOXEL_MARCH_BOUND 1 (and
		// hole stats on); see VoxelMarchBound.ush for the per-ray statics
		// and VoxelMarch.usf for the pre-falsifier snapshot they fold from.
		//
		//   BoundConsulted   every (segment, WalkL) pair the clamp was
		//                    actually asked about -- arm on, registers
		//                    loaded, interval non-degenerate. THE
		//                    DENOMINATOR. Zero while voxel.March.Bound reads
		//                    1 is ARMED AND INERT, the house failure, and
		//                    must never be spellable as "it cut nothing".
		//                    The Stage 0b task named only the three words
		//                    below; this one exists because without it those
		//                    three read identically for "inert" and for
		//                    "engaged, changed nothing" -- the ambiguity the
		//                    ZCut trio's header note refuses.
		//   BoundSegmentsSkipped  pairs whose interval came back EMPTY: the
		//                    walk never ran. THE PRIMARY COUNTER -- the
		//                    census put the removable mass in pre-hit ring
		//                    segments with zero resident chunks.
		//   BoundWalkInRaised / BoundWalkOutLowered  pairs the clamp
		//                    NARROWED without emptying, one per end.
		//                    Deliberately NOT a partition: one consult can
		//                    raise AND lower, so the pair can overlap each
		//                    other (never SegmentsSkipped), and
		//                    consulted - skipped covers the rest.
		//
		// DECISIONS, NOT NANOSECONDS -- every warning the ZCut trio carries
		// applies unchanged. THE SAVING IS `VoxelMarch.March` FROM ProfileGPU
		// (net of the producer's own boundMs bracket) AND NOTHING ELSE.
		BoundConsulted,
		BoundSegmentsSkipped,
		BoundWalkInRaised,
		BoundWalkOutLowered,
		End                                    // the buffer's word count while the census lives here
	};
}
// The kernel folds the stats buffer with one word per thread of an 8x8 group
// (`if (GroupIndex < VOXEL_MARCH_HOLE_WORDS)`), so the layout must fit in one
// group's threads or the tail words are silently never flushed -- plausible
// zeros, the exact incident VoxelMarchHoleWord's own note records.
static_assert(int32(VoxelMarchRayBoundWord::End) <=
                  kVoxelMarchTileSize * kVoxelMarchTileSize,
              "hole-stats words no longer fit one 8x8 group's flush");

// HALF-RESOLUTION MARCHING (voxel.March.HalfRes). Carried by every shader that
// touches the VisBuffer -- the march that writes it, BOTH vertex shaders (the
// emit tile is 16x16 full-res pixels instead of 8x8, and that is a compile-time
// scale in the quad), both pixel shaders that read it, and the depth gate.
//
// EVERY ONE OF THOSE, OR NONE. The buffer is physically half-size on this arm,
// so a shader compiled for the wrong side of the switch does not render
// slightly differently -- it indexes a texture that is not the size it thinks,
// which is the silent-wrong-answer failure this file keeps naming. The host
// therefore derives the emit-side value from the VisBuffer's OWN EXTENT rather
// than from a second read of the cvar; see the emit hook.
class FVoxelMarchHalfResDim : SHADER_PERMUTATION_BOOL("VOXEL_MARCH_HALFRES");

// THE COARSE OCCUPANCY LEVEL (voxel.March.BlockSkip). A PERMUTATION for the
// reason recorded at the cvar and NOT the reason voxel.March.ZCut is a uniform:
// this arm adds two buffer loads per 4^3 block, so a runtime branch would leave
// that traffic in the off arm's binary and the control would pay for memory it
// never reads. 0 is byte-identical to the pre-block kernel.
class FVoxelMarchBlockSkipDim : SHADER_PERMUTATION_BOOL("VOXEL_MARCH_BLOCK_SKIP");
// The retry ladder's gate (voxel.March.SkyLadder). A PERMUTATION for the reason
// block skip is one: it puts a field on the chunk cache and a fold at five walk
// sites, and a control carrying the treatment's registers is not a control.
// Only meaningful with rings AND fallthrough > 0; the other combinations are
// refused in ShouldCompilePermutation rather than built and never selected.
class FVoxelMarchSkyLadderDim : SHADER_PERMUTATION_BOOL("VOXEL_MARCH_SKY_LADDER");
// THE PER-RAY RESIDENT-EXTENT BOUND (voxel.March.Bound, Stage 0b). A
// PERMUTATION AND NOT A UNIFORM, on BlockSkip's side of that argument: the
// consumer adds up to seven Texture2DArray loads per ray, and a runtime
// branch would leave that traffic in the control's binary and flatter the arm
// under test. 0 is byte-identical to the pre-bound kernel -- no texture
// global, no statics, no clamp ("off is not merely unread, the global does
// not exist"). Rings only; refused against SkyLadder and HalfRes in
// ShouldCompilePermutation, with matching #errors in VoxelMarchBound.ush for
// anyone who bypasses the host -- see that file for both arguments.
class FVoxelMarchBoundDim : SHADER_PERMUTATION_BOOL("VOXEL_MARCH_BOUND");

// ===========================================================================
// THE WALK SHAPE, AND WHY IT IS A STRUCT WITH A COUNT NAILED TO IT
// ===========================================================================
//
// THE BUG THIS EXISTS TO MAKE IMPOSSIBLE, stated as the shape rather than as the
// instance, because the instance has now happened three times in one file in one
// night:
//
//   1. the source comparator called the single-level walks directly, so with
//      rings on EVERY ring number was computed by a non-ring comparator;
//   2. that was fixed by routing the comparator through VoxelMarchTraverseRings;
//   3. Phase 6 then added VoxelMarchTraverseWithCover as a NEW OUTER LAYER above
//      the entry point that fix had just corrected -- and the comparator lagged
//      again, one level higher.
//
// THE FIX AND THE REOPENING WERE THE SAME SHAPE AT DIFFERENT HEIGHTS. Closing a
// gap at layer N does nothing to prevent layer N+1, and nothing in the codebase
// made that visible. This does.
//
// AND IT FAILS TOWARD FALSE AGREEMENT, which is the dangerous direction: both
// comparator arms exclude the new layer equally, so it produces no false
// disagreements -- it produces "the marcher is correct" about a picture that
// omits everything the new layer draws. Four instruments tonight failed by
// agreeing with what they existed to catch.
//
// HOW THIS FORCES THE ISSUE: every dimension that changes the SHAPE OF THE WALK
// gets a field here and is counted. The static_assert ties the field count to
// kVoxelMarchWalkShapeDims, so ADDING A DIMENSION WITHOUT CLASSIFYING IT IS A
// COMPILE ERROR rather than a silent omission. Whoever adds one must bump the
// count, which lands them in VoxelMarchComparatorShapeAgrees below, where they
// have to say whether the comparator MUST MATCH it or DELIBERATELY VARIES it.
// There is no third answer and no way to skip the question.
//
// Source, SkipLevels, Rings, Cover, CoverSkip, Fallthrough, BlockSkip,
// SkyLadder, Bound.
constexpr int32 kVoxelMarchWalkShapeDims = 9;

// THE COVER REACH, IN ONE PLACE. Both the shader binding and the comparator
// guard read this; two spellings of "is cover in the picture" is how a guard
// ends up disagreeing with the thing it guards.
//
// ZERO UNTIL THE COVER RING IS DRIVEN. Cover publication into the pool exists,
// but there is no host define for VOXEL_MARCH_COVER yet, so marching a cover
// segment would walk an empty grid slot and cost a second near-field traversal
// for nothing.
static float VoxelMarchCoverReachUU()
{
	return 0.0f;
}


struct FVoxelMarchWalkShape
{
	int32 Source = 0;
	int32 SkipLevels = 0;
	int32 Rings = 0;
	int32 Cover = 0;
	int32 CoverSkip = 0;
	// Fine -> coarse fallthrough depth (VOXEL_MARCH_FALLTHROUGH). MUST MATCH:
	// a fallthrough walk can HIT where a non-fallthrough walk holes, so a
	// comparator at a different depth grades a different picture. Carried by
	// FVoxelMarchFallthroughDim in the comparator's own permutation domain,
	// exactly as Rings is.
	int32 Fallthrough = 0;
	// The coarse occupancy level (VOXEL_MARCH_BLOCK_SKIP). A WALK-SHAPE
	// DIMENSION AND NOT AN OBSERVATION ONE, and the distinction was checked
	// rather than assumed: hole stats is exempt because nothing under it can
	// change a ray's bHit or THitUU, and this CAN. A skipped block sets
	// bCrossedAbsentChunk at BLOCK granularity rather than at the granularity of
	// the chunks the ray actually crossed, which changes which rays the
	// fallthrough ladder retries, and a retried ray can HIT where it previously
	// holed. The direction is safe -- more retries, never fewer -- but "safe"
	// is not "identical", and a comparator grading a different picture is the
	// failure this struct exists to make impossible.
	int32 BlockSkip = 0;
	// The retry ladder's gate (VOXEL_MARCH_SKY_LADDER). A WALK-SHAPE DIMENSION,
	// and the classification is not a formality: this switch decides WHICH RAYS
	// RETRY at a coarser level, and a retried ray can HIT where it previously
	// holed. So it changes bHit and THitUU, which is exactly the test that
	// separates a walk-shape dimension from an observation one -- hole stats is
	// exempt because nothing under it can change a hit, and this can.
	//
	// MUST MATCH, not DELIBERATELY VARIED: a comparator run at a different gate
	// grades a different picture, which is the failure the walk-shape struct
	// exists to make impossible. And the comparator's own permutation domain
	// does NOT carry this dimension, so with the arm on the two must be reported
	// as incomparable rather than compared -- see the check below.
	int32 SkyLadder = 0;
	// The per-ray resident-extent bound (VOXEL_MARCH_BOUND). A WALK-SHAPE
	// DIMENSION, and the classification was checked rather than assumed
	// against the same test BlockSkip and SkyLadder passed: this CAN change
	// bHit. Not through the removed space -- nothing there can validate a hit
	// -- but through the 512-chunk cap and the step budget: a walk that no
	// longer burns hundreds of steps crossing non-resident space can REACH
	// AND HIT ground the control's walk capped out before (the safe
	// direction, more terrain, but "safe" is not "identical"). It also folds
	// bCrossedAbsentChunk at interval granularity, exactly as BlockSkip's
	// note describes for blocks. MUST MATCH; the comparator's domain does not
	// carry it, so with the arm on the two are incomparable -- see the check.
	int32 Bound = 0;
};
static_assert(sizeof(FVoxelMarchWalkShape) == kVoxelMarchWalkShapeDims * sizeof(int32),
              "a walk-shape dimension was added or removed without updating "
              "kVoxelMarchWalkShapeDims. Bump it, then go to "
              "VoxelMarchComparatorShapeAgrees and classify the new dimension as one the "
              "comparator MUST MATCH or one it DELIBERATELY VARIES. Adding a layer to the walk "
              "without answering that question is how the comparator silently stopped describing "
              "what the marcher draws -- three times in this file.");

class FVoxelMarchCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchCS, FGlobalShader);
	using FParameters = FVoxelMarchCSParameters;
	using FPermutationDomain =
		TShaderPermutationDomain<FVoxelMarchSourceDim, FVoxelMarchSkipDim, FVoxelMarchRingsDim,
		                         FVoxelMarchFallthroughDim, FVoxelMarchHoleStatsDim,
		                         FVoxelMarchHalfResDim, FVoxelMarchBlockSkipDim,
		                         FVoxelMarchSkyLadderDim, FVoxelMarchBoundDim>;

	// One group == one tile, non-negotiable: the group's hit reduction is what
	// fills the emit's tile list.
	static constexpr int32 kGroupSize = kVoxelMarchTileSize;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		if (!IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5))
		{
			return false;
		}
		// The occupancy source has no hierarchy to skip over -- it is one flat
		// 10 cm grid. Compiling skip > 0 against it would produce a permutation
		// that builds, binds and silently means nothing.
		const FPermutationDomain P(Parameters.PermutationId);
		// The rings arm reads a per-level chunk index and rescales the ray by
		// 2^level; the occupancy source is one flat volume with no levels at
		// all, so rings against it would build, bind and mean nothing.
		if (P.Get<FVoxelMarchRingsDim>() && P.Get<FVoxelMarchSourceDim>() == 0)
		{
			return false;
		}
		// Fallthrough is a property of the ring walk and nothing else reads the
		// define, so a depth > 0 permutation without rings would build, bind
		// and mean nothing -- refused for the reason the pair above is.
		if (P.Get<FVoxelMarchFallthroughDim>() > 0 && !P.Get<FVoxelMarchRingsDim>())
		{
			return false;
		}
		// The sky ladder GATES the fallthrough ladder, so without a ladder there
		// is nothing for it to gate: the define would compile a second flag,
		// fold it at five sites, and have no reader. Refused rather than built,
		// for the reason the clause above is -- and it also HALVES the
		// permutation count this dimension would otherwise add, since every
		// fallthrough-0 and rings-off combination is now excluded.
		if (P.Get<FVoxelMarchSkyLadderDim>() &&
		    (P.Get<FVoxelMarchFallthroughDim>() == 0 || !P.Get<FVoxelMarchRingsDim>()))
		{
			return false;
		}
		// The hole counters are ring-walk properties (substituted compares the
		// walked level against the owning segment; uncovered needs per-level
		// residency), so hole stats without rings would build, bind and mean
		// nothing -- refused for the reason the pair above is. Rings already
		// imply source 1, so that pairing needs no third clause.
		if (P.Get<FVoxelMarchHoleStatsDim>() && !P.Get<FVoxelMarchRingsDim>())
		{
			return false;
		}
		// The coarse occupancy level is consulted by the RING walk's chunk loop
		// and by nothing else, so block skip without rings would build, bind and
		// mean nothing -- refused for the reason the three clauses above are.
		if (P.Get<FVoxelMarchBlockSkipDim>() && !P.Get<FVoxelMarchRingsDim>())
		{
			return false;
		}
		// The per-ray resident-extent bound (voxel.March.Bound). Three
		// refusals, each restated as an #error in VoxelMarchBound.ush:
		//   * without RINGS the clamp has no socket -- it lives in the ring
		//     walk's segment loop and nothing else reads the define;
		//   * with SKY LADDER the ladder's gate reads bWalkTruncated, which
		//     the bound eliminates on exactly the rays that retry today --
		//     the ladder would CLOSE on them and drop fills;
		//   * with HALF RES the march samples a jittered block-centre
		//     lattice while the producer rasterises pixel centres, so the
		//     bound would describe a different ray than the one marched.
		if (P.Get<FVoxelMarchBoundDim>() &&
		    (!P.Get<FVoxelMarchRingsDim>() || P.Get<FVoxelMarchSkyLadderDim>() ||
		     P.Get<FVoxelMarchHalfResDim>()))
		{
			return false;
		}
		return P.Get<FVoxelMarchSourceDim>() != 0 || P.Get<FVoxelMarchSkipDim>() == 0;
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_TILE_SIZE"), kVoxelMarchTileSize);
		// The hole-stats word layout, pushed from the ONE enum in the header
		// rather than restated in the .usf -- see VoxelMarchHoleWord for the
		// incident (silently-discarded out-of-range writes reading back as
		// plausible values) that makes a hand mirror unacceptable here.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_RAYS"),
		                         int32(VoxelMarchHoleWord::Rays));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HITS"),
		                         int32(VoxelMarchHoleWord::Hits));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_SUBSTITUTED"),
		                         int32(VoxelMarchHoleWord::Substituted));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_UNCOVERED"),
		                         int32(VoxelMarchHoleWord::Uncovered));
		// The fallthrough ladder, so the shader can name the two words the CPU
		// reads back. Without these the counters compile to nothing and the
		// shell gate is unprovable -- see FallthroughConsidered in the header.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_FT_CONSIDERED"),
		                         int32(VoxelMarchHoleWord::FallthroughConsidered));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_FT_TAKEN"),
		                         int32(VoxelMarchHoleWord::FallthroughTaken));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_UNCOVERED_SHELL"),
		                         int32(VoxelMarchHoleWord::UncoveredShell));
		// THE LEVEL-GROUP SIZE, PUSHED RATHER THAN WRITTEN. The kernel's
		// sentinel guard was the literal `< 6u` while this group already held
		// seven words, so a level-6 miss (the 8 km ring) was silently dropped
		// into the shortfall the perf line reports as "capture missed rays".
		// One authority, and it moves when the enum does.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_NUM_LEVELS"),
		                         VoxelMarchHoleWord::kNumLevels);
		// The level-2 breakdown's two word groups: 6 level words then 4
		// reason words. The shader adds the ring level / the
		// VOXEL_MARCH_MISS_* code to these bases, so the group sizes are
		// layout shared with the enum -- static_asserted right below the
		// class rather than trusted.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_UNC_LEVEL0"),
		                         int32(VoxelMarchHoleWord::UncLevelFirst));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_UNC_REASON0"),
		                         int32(VoxelMarchHoleWord::UncReasonFirst));
		// The resident-Z bound's engagement trio (voxel.March.ZCut), pushed
		// from the same enum for the same reason every other word is: a hand
		// mirror here reads a plausible number out of the wrong slot, which is
		// the incident VoxelMarchHoleWord's own note records.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_ZCUT_CONSULTED"),
		                         int32(VoxelMarchHoleWord::ZCutConsulted));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_ZCUT_SKIPPED"),
		                         int32(VoxelMarchHoleWord::ZCutSkipped));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_ZCUT_CLIPPED"),
		                         int32(VoxelMarchHoleWord::ZCutClipped));
		// The coarse occupancy level's engagement trio (voxel.March.BlockSkip),
		// pushed from the same enum for the same reason every other word is.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_BLK_CONSULTED"),
		                         int32(VoxelMarchHoleWord::BlockConsulted));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_BLK_SKIPPED"),
		                         int32(VoxelMarchHoleWord::BlockSkipped));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_BLK_SKY_LICENSED"),
		                         int32(VoxelMarchHoleWord::BlockSkyLicensed));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_BLK_SKY_RUNBLOCKS"),
		                         int32(VoxelMarchHoleWord::BlockSkyRunBlocks));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_BLK_SKY_RUNS"),
		                         int32(VoxelMarchHoleWord::BlockSkyRuns));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_BLK_CELLS"),
		                         int32(VoxelMarchHoleWord::BlockCellsAvoided));
		// The height pyramid's engagement group, pushed from the same enum for
		// the same reason every other word is: a hand mirror here reads a
		// plausible number out of the wrong slot, which is the incident
		// VoxelMarchHoleWord's own note records.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HGT_CONSULTED"),
		                         int32(VoxelMarchHoleWord::HeightConsulted));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HGT_ADVANCED"),
		                         int32(VoxelMarchHoleWord::HeightAdvanced));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HGT_EMPTY"),
		                         int32(VoxelMarchHoleWord::HeightEmpty));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HGT_LEAFCELLS"),
		                         int32(VoxelMarchHoleWord::HeightLeafCells));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HGT_STEPS"),
		                         int32(VoxelMarchHoleWord::HeightSteps));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HGT_REENTRIES"),
		                         int32(VoxelMarchHoleWord::HeightReentries));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HGT_MISSED"),
		                         int32(VoxelMarchHoleWord::HeightMissed));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HGT_LATE"),
		                         int32(VoxelMarchHoleWord::HeightLate));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HGT_LATE_MAXUU"),
		                         int32(VoxelMarchHoleWord::HeightLateMaxUU));
		// The distribution's first bucket; the shader adds 0..4 to this base,
		// so the group's CONTIGUITY is layout shared with the enum and is
		// static_asserted below rather than trusted.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_HGT_LATE_B0"),
		                         int32(VoxelMarchHoleWord::HeightLateB0));
		// The block grid's own shape, pushed rather than written in the .ush,
		// under exactly the discipline the word layout above follows: the CPU
		// sizes and fills these bitfields and the shader indexes them, and a
		// hand-mirrored 32,768 in the shader against a widened index grid on the
		// CPU reads another slot's bits -- ground claimed empty that is not,
		// which is a hole with every counter healthy.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_BLOCKS_PER_SLOT"),
		                         int32(FVoxelMarchChunkIndex::kBlocksPerSlot));
		// THE BLOCK SIZE ITSELF, PUSHED SO THE SHADER CANNOT DRIFT FROM IT.
		// VoxelMarchIndexBlockCompute spells the divide as `>> 2` -- the cheap,
		// correct form for 4 and silently WRONG for any other value -- and the
		// traversal spells `ChunkCoord >> 2` and a +/-4 neighbour step the same
		// way. Raising kBlockChunks on the CPU without those would publish bits
		// for one block size and read them at another: ground claimed empty that
		// is not, i.e. holes, with every counter healthy. The .ush turns this
		// into an #error rather than trusting anyone to notice.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_BLOCK_CHUNKS"),
		                         int32(FVoxelMarchChunkIndex::kBlockChunks));
		// The chunk-cell lookup census (VoxelMarchLookupChunk's return-site
		// partition), pushed from the same enum for the same reason every
		// other word is: a hand mirror here reads a plausible number out of
		// the wrong slot.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_CELLS_PROBED"),
		                         int32(VoxelMarchHoleWord::CellsProbed));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_CELL_NONRESIDENT"),
		                         int32(VoxelMarchHoleWord::CellNonResident));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_CELL_INDEX_EMPTY"),
		                         int32(VoxelMarchHoleWord::CellIndexEmpty));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_CELL_SLOT_REJECT"),
		                         int32(VoxelMarchHoleWord::CellSlotReject));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_CELL_STALE"),
		                         int32(VoxelMarchHoleWord::CellStale));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_CELL_RESIDENT_EMPTY"),
		                         int32(VoxelMarchHoleWord::CellResidentEmpty));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_CELL_BRICK_OOB"),
		                         int32(VoxelMarchHoleWord::CellBrickOOB));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_CELL_REAL"),
		                         int32(VoxelMarchHoleWord::CellReal));
		// The ray-bound census (Stage 0a), pushed from VoxelMarchRayBoundWord
		// -- the one authority for these appended words -- for the same reason
		// every group above is: a hand mirror here reads a plausible number
		// out of the wrong slot.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_RB_PROBES_UP"),
		                         int32(VoxelMarchRayBoundWord::ProbesUp));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_RB_PROBES_DOWN"),
		                         int32(VoxelMarchRayBoundWord::ProbesDown));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_RB_PRE_UP"),
		                         int32(VoxelMarchRayBoundWord::PreUp));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_RB_PRE_DOWN"),
		                         int32(VoxelMarchRayBoundWord::PreDown));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_RB_POST_UP"),
		                         int32(VoxelMarchRayBoundWord::PostUp));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_RB_POST_DOWN"),
		                         int32(VoxelMarchRayBoundWord::PostDown));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_RB_CAP_RAYS"),
		                         int32(VoxelMarchRayBoundWord::CapRays));
		// The bound's engagement group (voxel.March.Bound, Stage 0b), pushed
		// from the same appended-word namespace for the same reason every
		// group above is: a hand mirror here reads a plausible number out of
		// the wrong slot.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_BND_CONSULTED"),
		                         int32(VoxelMarchRayBoundWord::BoundConsulted));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_BND_SEGMENTS_SKIPPED"),
		                         int32(VoxelMarchRayBoundWord::BoundSegmentsSkipped));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_BND_WALKIN_RAISED"),
		                         int32(VoxelMarchRayBoundWord::BoundWalkInRaised));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_BND_WALKOUT_LOWERED"),
		                         int32(VoxelMarchRayBoundWord::BoundWalkOutLowered));
		// WORDS covers the appended census -- VoxelMarchRayBoundWord::End, not
		// the enum's Count, while the census lives outside the enum. Every
		// sizing of the stats buffer (create, copy, lock) reads End too.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_WORDS"),
		                         int32(VoxelMarchRayBoundWord::End));
	}
};
// The breakdown's group sizes ARE the buffer layout (the shader indexes
// base + level and base + reason code), so the enum must agree with itself
// before anything is allowed to read a word by arithmetic.
static_assert(int32(VoxelMarchHoleWord::UncReasonFirst) -
                      int32(VoxelMarchHoleWord::UncLevelFirst) ==
                  VoxelMarchHoleWord::kNumLevels,
              "hole-stats level group size drifted from the enum layout");
// PINNED AGAINST ZCutConsulted, NOT AGAINST Count, SINCE 2026-08-25. This
// assert used to read `Count - UncReasonFirst == kNumReasons`, which silently
// encoded "the reason group is LAST". It is not any more -- the resident-Z
// bound's three words were appended after it -- and reading it against Count
// would now fire on a correct layout while saying nothing about the group it
// exists to guard. The first word AFTER the group is the group's real end, so
// that is what it names.
static_assert(int32(VoxelMarchHoleWord::ZCutConsulted) -
                      int32(VoxelMarchHoleWord::UncReasonFirst) ==
                  VoxelMarchHoleWord::kNumReasons,
              "hole-stats reason group size drifted from the enum layout");
// AND THE APPENDED GROUP IS PINNED TOO, so the next person to append gets the
// same protection the reason group just lost and got back. Three words, and
// Count sits immediately past them.
static_assert(int32(VoxelMarchHoleWord::BlockConsulted) -
                      int32(VoxelMarchHoleWord::ZCutConsulted) == 3,
              "the voxel.March.ZCut word group is not three words. PINNED AGAINST THE FIRST "
              "WORD AFTER IT, not against Count, since 2026-08-25: it read against Count "
              "until voxel.March.BlockSkip appended its own trio, at which point it would "
              "have fired on a correct layout while saying nothing about the group it "
              "guards -- the same drift the reason group's assert had already been through.");
// AND THE NEWEST GROUP IS PINNED THE SAME WAY, against Count because it is
// currently last. WHOEVER APPENDS NEXT must repoint this at their first word,
// exactly as the two asserts above were repointed, or it silently stops
// guarding anything.
static_assert(int32(VoxelMarchHoleWord::BlockSkyLicensed) -
                      int32(VoxelMarchHoleWord::BlockConsulted) == 3,
              "the voxel.March.BlockSkip word group is not three words. REPOINTED "
              "2026-08-25 from Count to BlockSkyLicensed when the sky-licence trio was "
              "appended -- it fired on a correct layout, which is the assert doing its "
              "job: it guards a GROUP SIZE, so it must be pinned against the first word "
              "AFTER the group, never against Count once anything can follow.");
// AND THE SKY-LICENCE TRIO IS PINNED THE SAME WAY. REPOINTED 2026-08-26 from
// Count to CellsProbed when the chunk-cell census was appended -- it fired on a
// correct layout, which is the assert doing its job for the FOURTH time (reason
// group, ZCut, BlockSkip, and now the census). Each time the build failed
// loudly rather than a counter silently reading a neighbouring group's value,
// which is the whole reason these exist.
static_assert(int32(VoxelMarchHoleWord::CellsProbed) -
                      int32(VoxelMarchHoleWord::BlockSkyLicensed) == 3,
              "the sky-licence word group is not three words, or something was appended "
              "after it without repointing this assert at the new group's first word");
// THE CHUNK-CELL CENSUS IS PINNED AGAINST THE HEIGHT PYRAMID'S FIRST WORD.
// It used to be pinned against Count, because it used to be last; the height
// pyramid's group was appended after it, and this assert was repointed at that
// group's first word AS ITS OWN COMMENT INSTRUCTED. The instruction now lives
// on the height group's assert below. WHOEVER APPENDS NEXT repoints THAT one.
//
// EIGHT WORDS, AND THE COUNT IS LOAD-BEARING IN A WAY THE OTHER GROUPS' ARE
// NOT: seven of them are outcome buckets that the perf line SUMS and compares
// against the eighth. A ninth word appended INSIDE this group rather than after
// it would join neither the sum nor the denominator and the identity would keep
// printing PASS while the census silently lost a return path -- so the size is
// asserted here as well as checked at runtime.
static_assert(int32(VoxelMarchHoleWord::HeightConsulted) -
                      int32(VoxelMarchHoleWord::CellsProbed) == 8,
              "the chunk-cell census word group is not eight words (one denominator plus "
              "seven outcomes, one per return site of VoxelMarchLookupChunk), or "
              "something was appended after it without repointing this assert at the new "
              "group's first word. If you added a return site to that function, it needs "
              "a bucket of its own AND a slot in the perf line's identity sum -- see "
              "VoxelWorldSubsystem.cpp's 'Voxel march cell census' block.");
// THE HEIGHT PYRAMID'S GROUP IS PINNED AGAINST Count, because it is now last.
// WHOEVER APPENDS NEXT repoints THIS one at their first word -- and repoints
// nothing else, because every group above is already pinned to a named word
// rather than to the end of the enum.
//
// SEVEN WORDS, AND THE SHAPE OF THE GROUP IS WHY THE SIZE IS ASSERTED. Three of
// them are the arm's engagement partition (consulted / advanced / fully empty),
// two are the cost side (leaf cells skipped, DDA steps), one separates the
// mid-ray skips from the tStart advance (reentries), and the last is the
// falsifier. A word appended INSIDE this group rather than after it would be
// read as one of those by VoxelMarchGetAndResetHoleStats, and the counter it
// displaced would keep printing a plausible number -- which is the incident
// VoxelMarchHoleWord's own note records and the reason none of these layouts
// may be mirrored by hand.
static_assert(int32(VoxelMarchHoleWord::Count) -
                      int32(VoxelMarchHoleWord::HeightConsulted) == 14,
              "the height pyramid's engagement group is not fourteen words, or something "
              "was appended after it without repointing this assert at the new group's "
              "first word. If you added a counter to the height walk it needs a word of "
              "its own here, a define in ModifyCompilationEnvironment, a read in the "
              "readback and a line in voxel.March.Stats -- all four, or it reports a "
              "structural zero.");

// THE BUCKET GROUP IS CONTIGUOUS AND THE SHADER INDEXES IT AS A BASE PLUS
// 0..4. Spelled here rather than trusted, for the reason every other layout in
// this file is asserted: a reordered enum would send four of the five decades
// into whatever words happened to follow, and the histogram would still print
// five plausible numbers.
static_assert(int32(VoxelMarchHoleWord::HeightLateB1) ==
                      int32(VoxelMarchHoleWord::HeightLateB0) + 1 &&
                  int32(VoxelMarchHoleWord::HeightLateB2) ==
                      int32(VoxelMarchHoleWord::HeightLateB0) + 2 &&
                  int32(VoxelMarchHoleWord::HeightLateB3) ==
                      int32(VoxelMarchHoleWord::HeightLateB0) + 3 &&
                  int32(VoxelMarchHoleWord::HeightLateB4) ==
                      int32(VoxelMarchHoleWord::HeightLateB0) + 4,
              "the late-delta histogram buckets are not five contiguous words starting at "
              "HeightLateB0; the shader indexes them as a base plus 0..4 and would scatter "
              "four decades into other counters.");

IMPLEMENT_GLOBAL_SHADER(FVoxelMarchCS, VOXEL_MARCH_USF, "VoxelMarchMain", SF_Compute);

class FVoxelMarchCompactCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchCompactCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchCompactCS, FGlobalShader);
	using FParameters = FVoxelMarchCompactParameters;

	static constexpr int32 kGroupSize = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_COMPACT_GROUP"), kGroupSize);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_TILE_SIZE"), kVoxelMarchTileSize);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchCompactCS, VOXEL_MARCH_USF, "VoxelMarchCompactTilesMain", SF_Compute);

// The emit's vertex shader expands one classified tile into one screen quad.
// No vertex buffer and no vertex factory -- SV_VertexID and SV_InstanceID are
// the whole input, exactly like the project's existing fullscreen-triangle
// passes, except that the instance is a TILE and not the screen.
class FVoxelMarchEmitVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchEmitVS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchEmitVS, FGlobalShader);
	using FParameters = FVoxelMarchEmitParameters;
	// THE VERTEX SHADER CARRIES THE HALF-RES BIT TOO, and it is not decoration:
	// the tile quad's size in screen pixels is VOXEL_MARCH_FULL_TILE_SIZE, which
	// is 8 at full res and 16 at half. A VS compiled for the wrong side would
	// rasterise a quarter of the tile it was handed and three quarters of the
	// terrain would simply not be drawn, with no error anywhere.
	using FPermutationDomain = TShaderPermutationDomain<FVoxelMarchHalfResDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_TILE_SIZE"), kVoxelMarchTileSize);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchEmitVS, VOXEL_MARCH_USF, "VoxelMarchEmitVS", SF_Vertex);

class FVoxelMarchAODim : SHADER_PERMUTATION_BOOL("VOXEL_MARCH_AO");
class FVoxelMarchDBufferDim : SHADER_PERMUTATION_BOOL("VOXEL_MARCH_DBUFFER");
class FVoxelMarchVelocityDim : SHADER_PERMUTATION_BOOL("VOXEL_MARCH_VELOCITY");

class FVoxelMarchEmitPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchEmitPS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchEmitPS, FGlobalShader);
	using FParameters = FVoxelMarchEmitParameters;
	using FPermutationDomain =
		TShaderPermutationDomain<FVoxelMarchSourceDim, FVoxelMarchAODim, FVoxelMarchDBufferDim,
		                         FVoxelMarchVelocityDim, FVoxelMarchRingsDim,
		                         FVoxelMarchHalfResDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		if (!IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5))
		{
			return false;
		}
		// THE DBUFFER PERMUTATION IS NOT COMPILED, and it is not compiled rather
		// than merely not selected. There is no binding for the DBuffer textures
		// from a game module (see THE DBUFFER HOLE in VoxelMarch.usf), so a
		// compiled-but-unbindable permutation would sit in the shader map
		// looking available and would fail at bind time on whichever leg first
		// tried it. When the binding exists, delete these three lines.
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FVoxelMarchDBufferDim>())
		{
			return false;
		}
		// Rings against the occupancy source: one flat volume, no levels. The
		// march CS refuses the same pair, and the two must agree or a leg picks
		// a PS whose companion CS does not exist.
		if (PermutationVector.Get<FVoxelMarchRingsDim>() &&
		    PermutationVector.Get<FVoxelMarchSourceDim>() == 0)
		{
			return false;
		}
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_TILE_SIZE"), kVoxelMarchTileSize);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchEmitPS, VOXEL_MARCH_USF, "VoxelMarchEmitPS", SF_Pixel);

// ---------------------------------------------------------------------------
// THE DEPTH PRE-EMIT. See the long note above VoxelMarchDepthOnlyPS.
//
// A DELIBERATELY MINIMAL PARAMETER STRUCT. It could have reused the emit's, but
// that one carries the brick pool, the occupancy volume and the chunk index --
// none of which this pass reads, and every unset RDG resource in a bound struct
// is a validation failure waiting for whoever adds the next parameter.
// ---------------------------------------------------------------------------
BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchDepthOnlyParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelMarchViewParameters, MarchView)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint2>, MarchVis)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, MarchHitT)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchTileList)
	RDG_BUFFER_ACCESS(MarchDrawArgs, ERHIAccess::IndirectArgs)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

// Same entry point as the emit's vertex shader -- the same tile quads over the
// same hit list -- but bound against the minimal struct above.
class FVoxelMarchDepthOnlyVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchDepthOnlyVS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchDepthOnlyVS, FGlobalShader);
	using FParameters = FVoxelMarchDepthOnlyParameters;
	// Same entry point as the emit's VS and therefore the same reason for the
	// dimension: the quad's size in screen pixels is a compile-time constant.
	using FPermutationDomain = TShaderPermutationDomain<FVoxelMarchHalfResDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_TILE_SIZE"), kVoxelMarchTileSize);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchDepthOnlyVS, VOXEL_MARCH_USF, "VoxelMarchEmitVS", SF_Vertex);

class FVoxelMarchDepthOnlyPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchDepthOnlyPS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchDepthOnlyPS, FGlobalShader);
	using FParameters = FVoxelMarchDepthOnlyParameters;
	// It must run the SAME reconstruction the emit runs, over the same four
	// samples, or the emit's equal-depth test fails against the depth this pass
	// wrote and terrain disappears in patches. One function in the shader, one
	// dimension here.
	using FPermutationDomain = TShaderPermutationDomain<FVoxelMarchHalfResDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_TILE_SIZE"), kVoxelMarchTileSize);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchDepthOnlyPS, VOXEL_MARCH_USF, "VoxelMarchDepthOnlyPS", SF_Pixel);

BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchVerifyParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelMarchViewParameters, MarchView)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint2>, MarchVis)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, MarchHitT)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, MarchSceneDepthTexture)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchOutVerify)
END_SHADER_PARAMETER_STRUCT()

class FVoxelMarchVerifyDepthCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchVerifyDepthCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchVerifyDepthCS, FGlobalShader);
	using FParameters = FVoxelMarchVerifyParameters;
	// THE GATE MUST GRADE THE RECONSTRUCTION, NOT THE MARCH. It keeps running at
	// FULL res -- one thread per screen pixel, the dispatch below is unchanged --
	// but on this arm it reaches the VisBuffer through the same reconstruction
	// the emit uses, so what it scores is the depth the emit would have written.
	// A gate that read the half-res samples directly would certify the march and
	// say nothing about the step that stands between the march and the picture.
	using FPermutationDomain = TShaderPermutationDomain<FVoxelMarchHalfResDim>;

	static constexpr int32 kGroupSize = kVoxelMarchTileSize;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_TILE_SIZE"), kVoxelMarchTileSize);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchVerifyDepthCS, VOXEL_MARCH_USF, "VoxelMarchVerifyDepthMain",
                        SF_Compute);

BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchVerifySourceParameters, )
	// BOTH sources, and the view block the ray setup needs. This is the one
	// parameter struct in the file that binds everything at once, because it is
	// the one kernel that runs everything at once.
	VOXEL_FLUID_OCCUPANCY_PARAMETERS()
	VOXEL_BRICK_POOL_PARAMETERS()
	SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelMarchViewParameters, MarchView)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchChunkIndex)
	// THE COARSE OCCUPANCY LEVEL (voxel.March.BlockSkip). One bit per 4x4x4
	// block of chunk cells: Occupied means at least one cell in the block is
	// resident, AnyAbsent means at least one is not. 32 KiB each against the
	// 64 MiB they bound.
	//
	// BOUND ON EVERY PERMUTATION, INCLUDING VOXEL_MARCH_BLOCK_SKIP 0, for the
	// reason the cover pair below is bound on every arm -- and here the stakes
	// are higher than a silent zero. An unbound Buffer<uint> reads as ZEROS,
	// zero in Occupied means "no chunk in this block is resident", and a
	// marcher that believed that would skip the entire world in one jump: every
	// ray a hole, no error anywhere. FVoxelMarchChunkIndex::RegisterWithBlocks
	// therefore guarantees a non-null pair whenever it returns an index at all,
	// falling back to an ALL-ONES grid (descend everywhere, i.e. the control)
	// rather than to nothing.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockOccupied)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockAnyAbsent)
	// THE SKY LICENCE (voxel.March.BlockSkySkip). One bit per block: all 64
	// cells carry a valid open-sky mark AND none is resident.
	//
	// BOUND ON EVERY PERMUTATION for the reason the pair above is, but its
	// FAILURE DIRECTION IS THE OPPOSITE and that is worth saying at the binding
	// rather than only at the writer. Unbound reads as zeros; zero here means
	// "no block is provably sky", so the arm licenses nothing and the marcher
	// descends exactly as the control does. INERT, not wrong -- which is why
	// this one falls back to all ZEROS while the occupancy pair falls back to
	// all ones.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockAllSky)
	// 0 residency-only / 1 both / 2 sky-only. A uniform rather than a
	// permutation -- see FVoxelMarchArm::BlockSkyMode for that argument. Bound
	// on every permutation because an unset uniform is a silent zero, and zero
	// is also the correct "sky licence off" value: the safe default and the
	// honest default are the same number here.
	SHADER_PARAMETER(uint32, MarchBlockSkyMode)
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
	SHADER_PARAMETER(uint32, MarchIndexCellsPerLevel)
	// PHASE 6. Bound on every permutation, including the ones compiled with
	// VOXEL_MARCH_COVER 0: a shader-side global with no entry in the struct is
	// an unbound parameter, and reach 0 is the honest "cover is off" value.
	SHADER_PARAMETER(uint32, MarchCoverIndexGrid)
	SHADER_PARAMETER(float, MarchCoverReachUU)
	// One bit per index grid slot: set means that slot holds at least one
	// resident chunk. Read ONLY by the voxel.March.HoleStats 2 permutation
	// (VoxelMarchAbsentTouchesShell), bound on every arm for the reason the
	// cover pair above is -- an unbound uniform is a silent zero, and a zero
	// here means every slot is empty, which would make every absent chunk read
	// as a hole rather than none.
	SHADER_PARAMETER(uint32, MarchIndexLevelPopulated)
	// THE RESIDENT-Z BOUND (voxel.March.ZCut). Bound on every permutation for
	// the reason the two above are: an unbound uniform is a silent zero, and
	// zero in MarchZCutEnable is the honest "do not cut" value, so the safe
	// default and the correct default are again the same number.
	//
	// One int4 per INDEX GRID SLOT, not per ring level -- the cover slot has
	// its own walk and is simply never given a usable bound.
	//   .x  min chunk Z, already padded outward
	//   .y  max chunk Z, already padded outward, INCLUSIVE
	//   .z  1 = this slot's bound is usable; 0 = DO NOT CUT this slot
	//   .w  unused, 0
	// Padding is applied CPU-side so the shader has one number to trust and
	// there is no second place for the pad to be forgotten.
	SHADER_PARAMETER(int32, MarchZCutEnable)
	SHADER_PARAMETER_ARRAY(FIntVector4, MarchLevelChunkZ, [8])
	SHADER_PARAMETER(uint32, MarchRingCount)
	SHADER_PARAMETER(float, MarchRing0OuterUU)
	SHADER_PARAMETER(FIntVector, MarchPackOriginVoxel)
	SHADER_PARAMETER(FIntVector, MarchBrickOriginVoxel)
	SHADER_PARAMETER(int32, MarchStepBudget)
	SHADER_PARAMETER(int32, MarchHasPrepassDepth)
	SHADER_PARAMETER(int32, MarchOccValid)
	SHADER_PARAMETER(int32, MarchMutateCounters)
	SHADER_PARAMETER(int32, MarchIndexResidency)
	SHADER_PARAMETER(int32, MarchIndexUploads)
	SHADER_PARAMETER(FUintVector2, MarchIndexHash)
	SHADER_PARAMETER(uint32, MarchFrameNumber)
	SHADER_PARAMETER(int32, MarchQuietFrames)
	SHADER_PARAMETER(int32, MarchPoolLevel0)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, MarchSceneDepthTexture)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchOutVerifySource)
END_SHADER_PARAMETER_STRUCT()

class FVoxelMarchVerifySourceCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchVerifySourceCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchVerifySourceCS, FGlobalShader);
	using FParameters = FVoxelMarchVerifySourceParameters;
	// THE COMPARATOR HAD NO PERMUTATION DOMAIN AND THAT WAS THE DEFECT.
	//
	// The rings dimension was added to the march CS and the emit PS and never to
	// this kernel, so the kernel that produces EVERY number was compiled
	// VOXEL_MARCH_RINGS = 0 permanently. One uncompiled permutation explained
	// three separate observations that were being investigated as independent
	// findings:
	//
	//   * "no ray enters ring 1"  -- there is no ring loop in a rings-off
	//     comparator, so RingsEntered was never set and every level read 0.
	//     Not occlusion, not budget, not reach.
	//   * "exactly half the rays miss the volume" -- the CPU moved the frame
	//     origin to the camera for the ring arm while THIS shader kept the
	//     cornered box, so the camera sat on the box CORNER and every ray with a
	//     negative dominant component exited at TExit == TEnter. The origin
	//     moved and the box did not.
	//   * "43x is a level-0 number" -- true, and now for a known reason.
	// Fallthrough rides the domain for the same reason rings had to: the
	// kernel that produces every number must walk the cascade the marcher
	// walks, and a dimension missing here is compiled 0 permanently -- the
	// exact defect the block above records.
	using FPermutationDomain =
		TShaderPermutationDomain<FVoxelMarchRingsDim, FVoxelMarchFallthroughDim>;

	static constexpr int32 kGroupSize = kVoxelMarchTileSize;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		if (!IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5))
		{
			return false;
		}
		// Same rule as the march CS: only the ring walk reads the define.
		const FPermutationDomain P(Parameters.PermutationId);
		return !(P.Get<FVoxelMarchFallthroughDim>() > 0 && !P.Get<FVoxelMarchRingsDim>());
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		// THE ONLY PLACE VOXEL_MARCH_VERIFY_SOURCE IS EVER SET. It compiles BOTH
		// traversals into one kernel, which no march or emit permutation does --
		// so this is also the only build that would catch a symbol collision
		// between the two implementations.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_SOURCE"), 1);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_VERIFY_SOURCE"), 1);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_TILE_SIZE"), kVoxelMarchTileSize);
		// The comparator's buffer is the SAME class of join that just detached on
		// the index probe -- kSourceCompareWords against VOXEL_MARCH_VSRC_WORDS,
		// verified until now by a script I ran by hand after every edit. Pushing
		// the allocation makes the shader refuse to compile if its own slot map
		// outgrows the buffer, which is a guarantee rather than a habit.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_VSRC_ALLOC_WORDS"),
		                         FVoxelMarchState::kSourceCompareWords);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchVerifySourceCS, VOXEL_MARCH_USF, "VoxelMarchVerifySourceMain",
                        SF_Compute);

BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchVerifyIndexParameters, )
	// BOTH sources bound: layer 2 of the probe counts the same 32^3 voxels
	// through each of them and compares. That comparison is the whole point --
	// the pool's count should be the LARGER, because m != MAT_AIR is a strict
	// superset of the fluid predicate.
	VOXEL_FLUID_OCCUPANCY_PARAMETERS()
	VOXEL_BRICK_POOL_PARAMETERS()
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchChunkIndex)
	// THE COARSE OCCUPANCY LEVEL (voxel.March.BlockSkip). One bit per 4x4x4
	// block of chunk cells: Occupied means at least one cell in the block is
	// resident, AnyAbsent means at least one is not. 32 KiB each against the
	// 64 MiB they bound.
	//
	// BOUND ON EVERY PERMUTATION, INCLUDING VOXEL_MARCH_BLOCK_SKIP 0, for the
	// reason the cover pair below is bound on every arm -- and here the stakes
	// are higher than a silent zero. An unbound Buffer<uint> reads as ZEROS,
	// zero in Occupied means "no chunk in this block is resident", and a
	// marcher that believed that would skip the entire world in one jump: every
	// ray a hole, no error anywhere. FVoxelMarchChunkIndex::RegisterWithBlocks
	// therefore guarantees a non-null pair whenever it returns an index at all,
	// falling back to an ALL-ONES grid (descend everywhere, i.e. the control)
	// rather than to nothing.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockOccupied)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockAnyAbsent)
	// THE SKY LICENCE (voxel.March.BlockSkySkip). One bit per block: all 64
	// cells carry a valid open-sky mark AND none is resident.
	//
	// BOUND ON EVERY PERMUTATION for the reason the pair above is, but its
	// FAILURE DIRECTION IS THE OPPOSITE and that is worth saying at the binding
	// rather than only at the writer. Unbound reads as zeros; zero here means
	// "no block is provably sky", so the arm licenses nothing and the marcher
	// descends exactly as the control does. INERT, not wrong -- which is why
	// this one falls back to all ZEROS while the occupancy pair falls back to
	// all ones.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBlockAllSky)
	// 0 residency-only / 1 both / 2 sky-only. A uniform rather than a
	// permutation -- see FVoxelMarchArm::BlockSkyMode for that argument. Bound
	// on every permutation because an unset uniform is a silent zero, and zero
	// is also the correct "sky licence off" value: the safe default and the
	// honest default are the same number here.
	SHADER_PARAMETER(uint32, MarchBlockSkyMode)
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
	SHADER_PARAMETER(uint32, MarchIndexCellsPerLevel)
	// PHASE 6. Bound on every permutation, including the ones compiled with
	// VOXEL_MARCH_COVER 0: a shader-side global with no entry in the struct is
	// an unbound parameter, and reach 0 is the honest "cover is off" value.
	SHADER_PARAMETER(uint32, MarchCoverIndexGrid)
	SHADER_PARAMETER(float, MarchCoverReachUU)
	// One bit per index grid slot: set means that slot holds at least one
	// resident chunk. Read ONLY by the voxel.March.HoleStats 2 permutation
	// (VoxelMarchAbsentTouchesShell), bound on every arm for the reason the
	// cover pair above is -- an unbound uniform is a silent zero, and a zero
	// here means every slot is empty, which would make every absent chunk read
	// as a hole rather than none.
	SHADER_PARAMETER(uint32, MarchIndexLevelPopulated)
	// THE RESIDENT-Z BOUND (voxel.March.ZCut). Bound on every permutation for
	// the reason the two above are: an unbound uniform is a silent zero, and
	// zero in MarchZCutEnable is the honest "do not cut" value, so the safe
	// default and the correct default are again the same number.
	//
	// One int4 per INDEX GRID SLOT, not per ring level -- the cover slot has
	// its own walk and is simply never given a usable bound.
	//   .x  min chunk Z, already padded outward
	//   .y  max chunk Z, already padded outward, INCLUSIVE
	//   .z  1 = this slot's bound is usable; 0 = DO NOT CUT this slot
	//   .w  unused, 0
	// Padding is applied CPU-side so the shader has one number to trust and
	// there is no second place for the pad to be forgotten.
	SHADER_PARAMETER(int32, MarchZCutEnable)
	SHADER_PARAMETER_ARRAY(FIntVector4, MarchLevelChunkZ, [8])
	SHADER_PARAMETER(uint32, MarchRingCount)
	SHADER_PARAMETER(float, MarchRing0OuterUU)
	SHADER_PARAMETER(FIntVector, MarchPackOriginVoxel)
	SHADER_PARAMETER(FIntVector, MarchVerifyIndexBaseChunk)
	// PHASE 6: 1 = probe the COVER grid slot at the cover level instead of ring 0.
	// A uniform rather than a permutation because this kernel traces no rays and
	// is dispatched once outside every timing bracket, so a branch cannot re-base
	// an arm -- and one leg being able to read BOTH grids is the point: "the cover
	// index is empty" and "the terrain index is empty" have different owners.
	SHADER_PARAMETER(int32, MarchVerifyIndexCover)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchOutVerifyIndex)
END_SHADER_PARAMETER_STRUCT()

class FVoxelMarchVerifyIndexCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchVerifyIndexCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchVerifyIndexCS, FGlobalShader);
	using FParameters = FVoxelMarchVerifyIndexParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		// The kernel exists ONLY under source 1 -- every symbol it reads is
		// declared there. Compiled with the define forced rather than as a
		// permutation, because there is no source-0 variant of it to select.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_SOURCE"), 1);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_TILE_SIZE"), kVoxelMarchTileSize);
		// THE PROBE'S BUFFER LAYOUT, PUSHED RATHER THAN MIRRORED. The same
		// pattern TILE_SIZE uses one line up, and for the same reason: the
		// hand-copied version of these silently detached (60 against 64) and the
		// driver discarded the overflow instead of failing.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_VIDX_HEADER"),
		                         FVoxelMarchState::kIndexProbeHeaderWords);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_VIDX_SAMPLE_WORDS"),
		                         FVoxelMarchState::kIndexProbeSampleWords);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_VIDX_SAMPLE_MAX"),
		                         FVoxelMarchState::kIndexProbeSampleMax);
		// What the CPU actually allocates. The shader fails to compile if its own
		// derived total exceeds it -- see the fit check in VoxelMarch.usf.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_VIDX_ALLOC_WORDS"),
		                         FVoxelMarchState::kIndexProbeWords);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchVerifyIndexCS, VOXEL_MARCH_USF, "VoxelMarchVerifyIndexMain",
                        SF_Compute);

// ---------------------------------------------------------------------------
// Binding the brick-pool half
// ---------------------------------------------------------------------------
//
// Called for BOTH sources. Under source 0 the pool's SRVs are bound but the
// compiled shader never references them (VoxelBrickTraverse.ush declares them
// only under VOXEL_MARCH_SOURCE 1), so they cost four assignments and nothing
// else. Binding them unconditionally is what keeps one parameter struct serving
// both permutations -- see the note on the struct.
//
// RETURNS FALSE ONLY WHEN SOURCE 1 CANNOT RUN. Two ways, both ordinary and
// neither an error: the pool has never flushed or holds nothing
// (BindShaderParameters says so), or the chunk index has never uploaded. Under
// source 0 it always returns true, because none of that is being read.
//
// THE INDEX RETURNING NULL IS NOT A DETAIL. An unbound SRV reads as zeros, zero
// has kResidentBit clear, and every lookup would miss -- so the marcher would
// render a perfectly empty world with no error anywhere. Declining the frame is
// the only safe response and it is counted as its own reason.
// ===========================================================================
// THE RING SPEC. One place decides the cascade's geometry; the shader, the
// frame anchoring and the ring-discordant classifier all read it from here.
// Three readers computing "the same" radii independently is precisely the
// two-authorities failure this workstream has eight doctrine entries about.
// ===========================================================================
struct FVoxelMarchRingSpec
{
	int32 Count = 1;
	// Ring 0's outer radius. EVERY OTHER BOUND IS DERIVED FROM IT, on both
	// sides: ring L covers [R0 * 2^(L-1), R0 * 2^L) and ring 0 covers [0, R0).
	// That is kDefaultRingPresets exactly -- 0-128, 128-256, 256-512, 512-1024,
	// 1024-2048, 2048-4096 m.
	//
	// ONE NUMBER RATHER THAN TWELVE UPLOADED BOUNDS. Twelve bounds are twelve
	// chances for the shader's idea of a ring to drift from the CPU's, and this
	// file has paid for that class of drift repeatedly. One number cannot
	// disagree with itself, and the traversal, the ring-discordant classifier
	// and the frame reach all read the same derivation.
	float Ring0OuterUU = 0.0f;
	float ReachUU = 0.0f;
	bool bEnabled = false;

	float InnerUU(int32 L) const { return (L <= 0) ? 0.0f : Ring0OuterUU * float(1 << (L - 1)); }
	float OuterUU(int32 L) const { return Ring0OuterUU * float(1 << L); }
};

// Seven rings to 8.19 km (level 6 landed 2026-08-23). Mirrors
// VOXEL_MARCH_MAX_RINGS in VoxelBrickTraverse.ush and
// FVoxelMarchChunkIndex::kLevels; all three must agree or the marcher walks a
// level the index does not carry.
static constexpr int32 kVoxelMarchMaxRings = 7;

// ===========================================================================
// CAN A RAY ARITHMETICALLY REACH THE RING WE CLAIM TO BE MEASURING?
// ===========================================================================
//
// At budget 886 the answer for ring 1 was NO, and had been all along: ring 1
// begins at 128 m = 1,280 level-0 voxels, and 886 < 1,280. Every ring-1 number
// was therefore zero for a reason no code change could have moved, while two
// other explanations were being investigated. THE ARITHMETIC WAS CHECKABLE
// BEFORE THE LEG AND NOBODY CHECKED IT.
//
// THE BUDGET IS PER TRAVERSAL CALL, NOT PER RAY, and that is load-bearing here:
// VoxelMarchTraverseBrick's step loop is bounded by MarchStepBudget and the ring
// loop calls it once per ring, so each ring gets its own full allowance. The
// binding constraint is therefore the SINGLE WORST RING, not the sum.
//
// Steps needed to cross ring L, in that ring's own voxels:
//   ring 0 spans R0, later rings span R0 * 2^(L-1); a level-L voxel is
//   10 * 2^L UU. So ring 0 costs R0/10 voxels and every later ring costs R0/20 --
//   1,280 and 640 at the 128 m preset. The cascade again: constant per ring.
// A 3D DDA crosses up to sqrt(3) times that on a diagonal ray, which is the
// number to budget for rather than the axis-aligned best case.
struct FVoxelMarchRingReach
{
	int32 WorstStepsForRing = 0;   // the most any single ring needs
	int32 FirstUnreachable = -1;   // -1 when every ring fits
};

static FVoxelMarchRingReach VoxelMarchCheckRingReach(const struct FVoxelMarchRingSpec& Rings,
                                                     int32 StepBudget);

static FVoxelMarchRingSpec VoxelMarchGetRingSpec()
{
	FVoxelMarchRingSpec S;
	S.bEnabled = CVarVoxelMarchRings.GetValueOnAnyThread() != 0;
	// 100 UU per metre. A voxel is 0.1 m = 10 UU; the ring presets are metres.
	S.Ring0OuterUU = FMath::Max(CVarVoxelMarchRingOuterM.GetValueOnAnyThread(), 1.0f) * 100.0f;
	if (!S.bEnabled)
	{
		// Filled even when off: an unset uniform is a silent zero, and a zero
		// outer radius would make the cascade one empty interval with no error.
		S.Count = 1;
		S.ReachUU = S.Ring0OuterUU;
		return S;
	}
	// 0 = follow the streaming cascade (see the cvar's help text and
	// FVoxelMarchChunkIndex::SetStreamedRingLevels for why the default cannot
	// be a second hand-set copy of the cascade depth).
	const int32 Requested = CVarVoxelMarchRingCount.GetValueOnAnyThread();
	S.Count = FMath::Clamp(
		Requested > 0 ? Requested : GetGlobalVoxelMarchChunkIndex().GetStreamedRingLevels(),
		1, kVoxelMarchMaxRings);
	S.ReachUU = S.OuterUU(S.Count - 1);
	return S;
}

static FVoxelMarchRingReach VoxelMarchCheckRingReach(const FVoxelMarchRingSpec& Rings,
                                                     int32 StepBudget)
{
	FVoxelMarchRingReach R;
	for (int32 L = 0; L < Rings.Count; ++L)
	{
		const double LenUU = double(Rings.OuterUU(L)) - double(Rings.InnerUU(L));
		const double VoxelUU = 10.0 * double(1 << L);
		// sqrt(3) for the diagonal case. Budgeting the axis-aligned best case is
		// how a ring ends up unreachable for most of the frame while looking
		// reachable on paper.
		const int32 Need = FMath::CeilToInt(LenUU / VoxelUU * 1.7320508);
		R.WorstStepsForRing = FMath::Max(R.WorstStepsForRing, Need);
		if (Need > StepBudget && R.FirstUnreachable < 0)
		{
			R.FirstUnreachable = L;
		}
	}
	return R;
}

// ===========================================================================
// DOES THE COMPARATOR WALK WHAT THE MARCHER WALKS?
// ===========================================================================
//
// REFUSE, LOUDLY, RATHER THAN REPORT WITH A CAVEAT. A caveat gets dropped the
// moment a number is copied into a document; a refusal cannot be. Same rule
// tools/voxel-leg-summary.ps1 applies when it declines the row for an
// invalidated leg.
//
// EVERY DIMENSION IS CLASSIFIED HERE, EXACTLY ONCE, AND THERE ARE ONLY TWO
// CLASSES. kVoxelMarchWalkShapeDims makes reaching this function mandatory when
// a dimension is added.
//
//   MUST MATCH        the comparator has to walk the same thing the marcher
//                     does, or its verdict is about a different picture.
//   DELIBERATELY VARIES  the comparator exists precisely to compare two values
//                     of this dimension, so a difference is the measurement.
//
// Source and SkipLevels are DELIBERATELY VARIED: VOXEL_MARCH_VERIFY_SOURCE
// compiles both traversals into one kernel and walks flat against hierarchical
// over the same rays in the same frame. That is the whole instrument.
//
// Rings, Cover, CoverSkip and Fallthrough MUST MATCH. Rings and Fallthrough
// are carried by the comparator's own permutation domain (a fallthrough walk
// can HIT where a non-fallthrough walk holes, so a comparator at a different
// depth would grade a different picture). Cover and CoverSkip are NOT -- the
// comparator calls VoxelMarchTraverseRings / VoxelMarchTraverseBrick directly
// and never routes through VoxelMarchTraverseWithCover, so with cover on it
// would compare two cover-free walks and report agreement about a picture that
// omits every cover voxel the marcher draws.
//
// WHY THE FIX IS EXCLUSION AND NOT "ROUTE THE COMPARATOR THROUGH THE COVER
// WALK", which is the obvious move and is wrong: it would change the POPULATION
// INSIDE A LIVE INSTRUMENT mid-investigation. The disagreement counts are
// currently the only handle on two open stepping defects, and folding cover into
// both arms would move those counts for a reason unrelated to stepping. That is
// a measurement-integrity argument and it outranks having one traversal entry
// point.
static bool VoxelMarchComparatorShapeAgrees(FString& OutWhy)
{
	FVoxelMarchWalkShape Shipping;
	Shipping.Source = 1;   // the comparator only runs on source 1 anyway
	Shipping.SkipLevels = CVarVoxelMarchSkipLevels.GetValueOnRenderThread();
	Shipping.Rings = (CVarVoxelMarchRings.GetValueOnRenderThread() != 0) ? 1 : 0;
	// COVER IS READ FROM THE REACH, NOT FROM A PERMUTATION, because there is no
	// host define for VOXEL_MARCH_COVER yet. A reach of zero means the cover
	// segment is empty whatever the permutation says, so this is the honest
	// "is cover in the picture" question rather than a proxy for it. When the
	// define arrives it is read here INSTEAD, and the static_assert above is what
	// makes whoever adds it come to this line.
	Shipping.Cover = (VoxelMarchCoverReachUU() > 0.0f) ? 1 : 0;
	Shipping.CoverSkip = 0;
	// The same gating VoxelMarchGetArm applies: rings off forces the dimension
	// to its control value, and the permutation for the other combination does
	// not exist.
	Shipping.Fallthrough = (Shipping.Rings != 0)
	                           ? FMath::Clamp(CVarVoxelMarchFallthrough.GetValueOnRenderThread(), 0, 2)
	                           : 0;
	// Same gating again: rings off forces the dimension to its control value and
	// the permutation for the other combination is refused at compile.
	Shipping.BlockSkip =
		(Shipping.Rings != 0 && CVarVoxelMarchBlockSkip.GetValueOnRenderThread() != 0) ? 1 : 0;
	// Same gating a third time: VoxelMarchGetArm forces this off without rings
	// and without a fallthrough depth, and the permutation for the other
	// combinations is refused at compile.
	Shipping.SkyLadder = (Shipping.Rings != 0 && Shipping.Fallthrough > 0 &&
	                      CVarVoxelMarchSkyLadder.GetValueOnRenderThread() != 0)
	                         ? 1
	                         : 0;
	// The same gating VoxelMarchGetArm applies (rings only, sky ladder wins a
	// conflict), so this reports the arm the march can actually select. The
	// half-res forcing is deliberately NOT restated here: the comparator
	// refuses to run at half res on its own grounds already, and a second
	// spelling of that gate is how two guards drift.
	Shipping.Bound = (Shipping.Rings != 0 && Shipping.SkyLadder == 0 &&
	                  CVarVoxelMarchBound.GetValueOnRenderThread() != 0)
	                     ? 1
	                     : 0;

	// What the comparator kernel is actually compiled with. Rings rides its
	// permutation domain; the other two do not exist in it at all, which is
	// exactly the gap this function reports.
	FVoxelMarchWalkShape Comparator;
	Comparator.Source = 1;
	Comparator.SkipLevels = Shipping.SkipLevels;   // deliberately varied inside the kernel
	Comparator.Rings = Shipping.Rings;             // carried by FVoxelMarchRingsDim
	Comparator.Cover = 0;                          // NOT in FVoxelMarchVerifySourceCS's domain
	Comparator.CoverSkip = 0;
	Comparator.Fallthrough = Shipping.Fallthrough; // carried by FVoxelMarchFallthroughDim
	// NOT in FVoxelMarchVerifySourceCS's domain, exactly like Cover -- so with
	// block skip on the comparator would compare two walks that BOTH lack it.
	Comparator.BlockSkip = 0;
	// NOT in FVoxelMarchVerifySourceCS's domain either, exactly like Cover and
	// BlockSkip.
	Comparator.SkyLadder = 0;
	// NOT in FVoxelMarchVerifySourceCS's domain either. Both comparator arms
	// would walk unclamped, so with the bound on it would grade a picture the
	// marcher is not drawing (the bound arm can HIT where an unclamped walk
	// caps out) -- false pass, the dangerous direction.
	Comparator.Bound = 0;

	if (Shipping.Bound != Comparator.Bound)
	{
		OutWhy = TEXT("the marcher is running the PER-RAY RESIDENT-EXTENT BOUND "
		              "(voxel.March.Bound 1) and the comparator is not -- the dimension is not "
		              "in FVoxelMarchVerifySourceCS's permutation domain. The clamp can let a "
		              "walk REACH AND HIT ground the unclamped walk's 512-chunk cap never got "
		              "to, so the two kernels would be grading DIFFERENT PICTURES; both of the "
		              "comparator's arms would run unclamped equally, which makes it a FALSE "
		              "PASS about a picture the marcher is not drawing -- not a false fail. Run "
		              "the comparator with voxel.March.Bound 0.");
		return false;
	}

	if (Shipping.SkyLadder != Comparator.SkyLadder)
	{
		OutWhy = TEXT("the marcher is running the SKY LADDER GATE (voxel.March.SkyLadder 1) and "
		              "the comparator is not -- that dimension is not in "
		              "FVoxelMarchVerifySourceCS's permutation domain at all. The gate decides "
		              "WHICH RAYS RETRY at a coarser level, and a retried ray can HIT where it "
		              "previously holed, so the two kernels would be grading DIFFERENT PICTURES. "
		              "Both of the comparator's arms would use the old gate equally, which makes "
		              "it a FALSE PASS about a picture the marcher is not drawing -- not a false "
		              "fail. Run the comparator with voxel.March.SkyLadder 0.");
		return false;
	}

	if (Shipping.Cover != Comparator.Cover)
	{
		OutWhy = FString::Printf(
			TEXT("the marcher is walking GROUND COVER (reach %.1f UU) and the comparator is not. "
			     "Both of its arms would exclude cover equally, so it would report AGREEMENT "
			     "about a picture that omits every cover voxel the marcher draws -- a false pass, "
			     "not a false fail. Run the comparator with voxel.Cover.March 0, or measure cover "
			     "with voxel.Cover.VerifyStore, which compares bytes rather than rays."),
			VoxelMarchCoverReachUU());
		return false;
	}
	if (Shipping.CoverSkip != Comparator.CoverSkip)
	{
		OutWhy = TEXT("the cover walk's skipping dial differs between the marcher and the "
		              "comparator.");
		return false;
	}
	// BLOCK SKIP: EXCLUSION, THE SAME ANSWER COVER GETS AND FOR THE SAME REASON.
	// The dimension is not in FVoxelMarchVerifySourceCS's permutation domain, so
	// with the arm on BOTH comparator walks would run without the coarse level
	// and report agreement about a picture the marcher is not drawing -- a false
	// PASS, which is the dangerous direction. Routing the comparator through the
	// arm instead is the obvious move and is refused for the reason it was
	// refused for cover: it would change the population inside a live instrument
	// whose disagreement counts are currently the only handle on two open
	// stepping defects.
	if (Shipping.BlockSkip != Comparator.BlockSkip)
	{
		OutWhy = TEXT("the marcher is walking the COARSE OCCUPANCY LEVEL (voxel.March.BlockSkip "
		              "1) and the comparator is not -- the dimension is not in its permutation "
		              "domain. Both of its arms would skip nothing, so it would report AGREEMENT "
		              "about a walk the marcher is not performing: a false pass, not a false "
		              "fail. Run the comparator with voxel.March.BlockSkip 0, which is also the "
		              "arm whose correctness it is being asked about.");
		return false;
	}
	return true;
}

template <typename ParametersType>
static bool VoxelMarchBindPool(FRDGBuilder& GraphBuilder, int32 Source, ParametersType& Params,
                               int32& OutIndexEntries)
{
	FVoxelBrickPool& Pool = GetGlobalVoxelBrickPool();
	FVoxelMarchChunkIndex& Index = GetGlobalVoxelMarchChunkIndex();
	OutIndexEntries = Index.GetNumEntries();

	const bool bPoolBound = Pool.BindShaderParameters(GraphBuilder, Params);
	// ONE CALL FOR THE INDEX AND ITS COARSE LEVEL, and that is the coherence
	// argument rather than a convenience: the three buffers are staged from one
	// snapshot of one shadow and consumed into this one graph, so no pass this
	// function's caller adds can see a block bitfield from a different
	// generation than the index it describes. Splitting this into two calls
	// would reintroduce exactly the window it exists to not have.
	const FVoxelMarchChunkIndex::FBuffers IndexBuffers = Index.RegisterWithBlocks(GraphBuilder);
	FRDGBufferRef IndexBuffer = IndexBuffers.Index;
	if (IndexBuffer != nullptr)
	{
		Params.MarchChunkIndex = GraphBuilder.CreateSRV(IndexBuffer, PF_R32_UINT);
		// NON-NULL WHENEVER THE INDEX IS -- RegisterWithBlocks guarantees it,
		// falling back to an all-ones grid rather than to nothing, because an
		// unbound Buffer<uint> reads as zeros and zeros here mean "nothing is
		// resident anywhere", i.e. skip the world. Checked anyway: this is the
		// one binding whose failure mode is every ray a hole.
		if (IndexBuffers.BlockOccupied != nullptr && IndexBuffers.BlockAnyAbsent != nullptr &&
		    IndexBuffers.BlockAllSky != nullptr)
		{
			Params.MarchBlockOccupied =
				GraphBuilder.CreateSRV(IndexBuffers.BlockOccupied, PF_R32_UINT);
			Params.MarchBlockAnyAbsent =
				GraphBuilder.CreateSRV(IndexBuffers.BlockAnyAbsent, PF_R32_UINT);
			// ALL THREE OR NONE. RegisterWithBlocks stages them from one shadow
			// snapshot under one generation stamp, so binding two of three would
			// hand the marcher a sky licence from one flush against a residency
			// picture from another -- and those two disagreeing is precisely the
			// state in which a block reads all-sky while a chunk has landed in
			// it. The three-way test is the seam where that is enforced.
			Params.MarchBlockAllSky =
				GraphBuilder.CreateSRV(IndexBuffers.BlockAllSky, PF_R32_UINT);
		}
		else
		{
			static bool bBlockBindComplained = false;
			if (!bBlockBindComplained)
			{
				bBlockBindComplained = true;
				UE_LOG(LogVoxelMarch, Error,
				       TEXT("Voxel march: the chunk index returned a buffer but no coarse "
				            "occupancy level, which RegisterWithBlocks is written to make "
				            "impossible. Any kernel compiled with VOXEL_MARCH_BLOCK_SKIP 1 "
				            "will now read an unbound Buffer<uint> as zeros, conclude that "
				            "no chunk anywhere is resident, and skip the entire world -- "
				            "every ray a hole with no other error. Run "
				            "voxel.March.BlockSkip 0 and rebuild the shaders."));
			}
		}
	}
	Params.MarchIndexDimChunks = FUintVector(FVoxelMarchChunkIndex::kDimXY,
	                                         FVoxelMarchChunkIndex::kDimXY,
	                                         FVoxelMarchChunkIndex::kDimZ);
	Params.MarchIndexCellsPerLevel = FVoxelMarchChunkIndex::kCellsPerLevel;
	// THE SKY LICENCE MODE, SET HERE -- in the one place that fills the pool
	// bindings, on EVERY arm including the ones that will never read it. An
	// unset uniform is a silent zero and this file has paid for that twice; zero
	// is also the correct "residency only" value, so the safe default and the
	// honest default coincide. Read from the ARM, not the cvar, so a kernel
	// compiled without VOXEL_MARCH_BLOCK_SKIP is handed 0 rather than a mode it
	// has no code for.
	Params.MarchBlockSkyMode = uint32(FMath::Clamp(VoxelMarchGetArm().BlockSkyMode, 0, 2));
	// ---- PHASE 6: ground cover -------------------------------------------
	//
	// FILLED HERE, IN THE ONE PLACE THAT FILLS THE POOL BINDINGS, and filled on
	// every arm including the ones that will never read them. An unset uniform is
	// a silent zero and this file has paid for that twice; a reach of zero is
	// also the correct "cover is off" value, so the safe default and the honest
	// default are the same number here.
	//
	// THE SLOT COMES FROM THE INDEX, NOT FROM A LITERAL. GridSlotForLevel is the
	// single authority for where cover lives in the grid, and the shader reads it
	// as this uniform precisely so there is no second spelling to drift.
	Params.MarchCoverIndexGrid = uint32(FVoxelMarchChunkIndex::kCoverGridSlot);
	// 0 UNTIL THE COVER RING IS DRIVEN. Cover publication into the pool is not
	// wired yet, so marching a cover segment would walk an empty grid slot and
	// cost a second near-field traversal for nothing. This is the value that
	// makes voxel.Cover.March's control arm the default rather than an option.
	Params.MarchCoverReachUU = VoxelMarchCoverReachUU();
	// ---- the hole breakdown's shell test (voxel.March.HoleStats 2) --------
	//
	// WHICH SLOTS HOLD ANYTHING, one bit each. The marcher's shell test asks
	// "is this absent chunk next to a chunk we hold" -- a good question for a
	// level that streams something and a meaningless one for a level that
	// streams nothing, and -VoxelMaxRingLevel=0 is exactly the second case for
	// levels 1-5. That run is the instrument's proving run (it must read
	// near-100% holes above L0), so an empty slot has to short-circuit the
	// test rather than silently answer "no holes here". The bit is what lets
	// the shader tell the two apart.
	//
	// FILLED ON EVERY ARM, level 1 and level 0 included, for the reason the
	// cover pair above is filled: an unbound uniform reads as zero, and zero
	// here is "everything is empty" -- the loudest possible wrong answer
	// rather than a quiet one, but still one nobody asked for.
	//
	// GetNumEntriesAtLevel TAKES A LEVEL AND MAPS IT ITSELF, so the cover slot
	// is reached through kCoverLevel and not by counting grid slots here --
	// GridSlotForLevel stays the single authority for the mapping.
	{
		uint32 PopulatedMask = 0u;
		for (uint32 L = 0; L < FVoxelMarchChunkIndex::kRingGrids; ++L)
		{
			if (Index.GetNumEntriesAtLevel(L) > 0)
			{
				PopulatedMask |= (1u << L);
			}
		}
		if (Index.GetCoverEntries() > 0)
		{
			PopulatedMask |= (1u << uint32(FVoxelMarchChunkIndex::kCoverGridSlot));
		}
		Params.MarchIndexLevelPopulated = PopulatedMask;
	}
	// ---- THE RESIDENT-Z BOUND (voxel.March.ZCut) --------------------------
	//
	// FILLED HERE, BESIDE THE POPULATED MASK, and deliberately from the SAME
	// object at the SAME point in the frame. The two answers have to agree
	// about which slots hold anything -- the shader refuses to cut a slot the
	// mask calls empty, because the shell test short-circuits there -- and the
	// only way to guarantee that is to read them from one index at one moment
	// rather than from two reads that could straddle a flush.
	//
	// THE ORDERING ARGUMENT, because a render-thread read of game-thread state
	// deserves one rather than a shrug. Every widening of this bound happens in
	// NoteObservedSpan, which runs IMMEDIATELY BEFORE the kResidentBit write
	// for the same chunk, on the game thread, inside Seed or ApplyDelta. Those
	// cells reach the GPU only through an upload that is enqueued after that
	// loop returns, so by the time the marcher can SEE a resident cell, the
	// widening that covers it was already published. This read happens later
	// still. The pad below is what covers the remaining slack rather than an
	// argument about memory order.
	{
		const bool bZCut = CVarVoxelMarchZCut.GetValueOnRenderThread() != 0;
		// AT LEAST 1, AND THE MINIMUM IS LOAD-BEARING -- see the cvar's help.
		// Pad 0 would let a cut chunk sit face-adjacent in Z to a resident one
		// and drop a bCrossedShellAbsent the retry ladder gates on.
		const int32 Pad = FMath::Max(CVarVoxelMarchZCutPadChunks.GetValueOnRenderThread(), 1);
		// The array is 8 int4s because the index carries 8 grid slots. Spelled
		// as a literal in the parameter struct and in VoxelBrickTraverse.ush
		// (int4 MarchLevelChunkZ[8]), so the one authority checks the two
		// spellings HERE rather than letting a widened index silently write
		// past the uniform.
		static_assert(FVoxelMarchChunkIndex::kGridSlots == 8,
		              "MarchLevelChunkZ is declared [8] in FVoxelMarchCSParameters and as "
		              "int4 MarchLevelChunkZ[8] in VoxelBrickTraverse.ush. The index grew a "
		              "slot; widen both, or the far slots read another slot's Z bound.");
		int32 UsableMask = 0;
		for (uint32 S = 0; S < FVoxelMarchChunkIndex::kGridSlots; ++S)
		{
			// .z = 0 IS THE DEFAULT AND MEANS "DO NOT CUT". Every slot is
			// written on every arm, including the cover slot (which has its own
			// walk and is never given a bound) and including the arm where the
			// cvar is off, so there is no path on which the shader reads a
			// stale or unset triple.
			FIntVector4 Bound(0, 0, 0, 0);
			if (bZCut && S < FVoxelMarchChunkIndex::kRingGrids)
			{
				int32 MinZ = 0;
				int32 MaxZ = 0;
				if (Index.GetResidentChunkZBound(int32(S), MinZ, MaxZ))
				{
					// OUTWARD ON BOTH ENDS, ALWAYS. A too-tight bound is a
					// hole; a too-wide one is only slower.
					Bound = FIntVector4(MinZ - Pad, MaxZ + Pad, 1, 0);
					UsableMask |= (1 << int32(S));
				}
			}
			Params.MarchLevelChunkZ[int32(S)] = Bound;
			GVoxelMarchZCutRanZMin[int32(S)].store(Bound.X, std::memory_order_relaxed);
			GVoxelMarchZCutRanZMax[int32(S)].store(Bound.Y, std::memory_order_relaxed);
		}
		Params.MarchZCutEnable = bZCut ? 1 : 0;
		// WHAT WAS ACTUALLY UPLOADED, stamped from the values just written
		// rather than re-read from the cvar, and printed beside the cvar by
		// voxel.March.Stats for the reason halfRes prints asked next to ran: a
		// cvar reads back whatever was typed, and that reading alone is what
		// let nine switches in this project sit armed and inert.
		//
		// SAY EXACTLY WHAT THIS PROVES AND NO MORE. This is the BIND, one step
		// before the dispatch, so it proves the uniform was FILLED -- not that
		// the pass ran (frames / marchMs say that) and not that the shader used
		// it (the engagement counters say that, and only they do).
		GVoxelMarchZCutRanEnable.store(bZCut ? 1 : 0, std::memory_order_relaxed);
		GVoxelMarchZCutRanUsableMask.store(UsableMask, std::memory_order_relaxed);
		GVoxelMarchZCutRanPad.store(Pad, std::memory_order_relaxed);
	}
	// Ring segments, near to far, as plain t intervals in UU. Filled even when
	// the rings permutation is off, for the same reason both sources' bindings
	// ride one struct: an unset uniform is a silent zero, and a zero outer
	// radius would make the whole cascade one empty interval with no error.
	{
		const FVoxelMarchRingSpec Rings = VoxelMarchGetRingSpec();
		Params.MarchRingCount = uint32(Rings.Count);
		Params.MarchRing0OuterUU = Rings.Ring0OuterUU;
	}

	if (Source == 0)
	{
		return true;
	}
	return bPoolBound && IndexBuffer != nullptr;
}

// ===========================================================================
// The extension
// ===========================================================================

FVoxelMarchRenderExtension::FVoxelMarchRenderExtension(
	const FAutoRegister& AutoRegister, UWorld* InWorld,
	TSharedPtr<FVoxelMarchState, ESPMode::ThreadSafe> InState)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
	, State(MoveTemp(InState))
{
}

bool FVoxelMarchRenderExtension::IsActiveThisFrame_Internal(
	const FSceneViewExtensionContext& Context) const
{
	// The gate, and the whole reason mode 0 is byte-identical to a build without
	// this file: declining here means not one of the three hooks is called.
	// THE BASE CLASS GATES TO THIS EXTENSION'S WORLD, and this call was MISSING.
	// The volume check below was masking it: State->Volume is fed by one world's
	// fluid subsystem, so the marcher never ran for a second world by accident.
	// Removing the volume requirement without this would swap a fluid coupling
	// for a cross-world bug -- marching one world's bricks against another
	// world's depth in PIE. FVoxelShadowMarchExtension makes the same call for
	// the same stated reason (VoxelShadowMarch.cpp:407-410).
	if (!FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context))
	{
		return false;
	}
	if (!State.IsValid() || VoxelMarchGetArm().Mode == 0)
	{
		return false;
	}
	// SOURCE-AWARE. Source 0 marches the occupancy volume and cannot run without
	// one; source 1 marches the brick pool and never touches it. Declining here
	// on a missing volume is what made terrain rendering require the water sim.
	if (!VoxelMarchNeedsOccupancyVolume(VoxelMarchGetArm()))
	{
		return true;
	}
	FScopeLock Guard(&State->Lock);
	return State->Volume.IsValid();
}

FVoxelMarchRenderExtension::FViewMarch* FVoxelMarchRenderExtension::FindView(const FSceneView* View)
{
	for (FViewMarch& Entry : Views)
	{
		if (Entry.ViewKey == View)
		{
			return &Entry;
		}
	}
	return nullptr;
}

void FVoxelMarchRenderExtension::RetireTimingQueries()
{
	// RETIRED BEFORE THE GATE, not after -- the rule VoxelFluidRender.cpp's
	// spike learned the hard way. If the cvar goes to 0 with pairs in flight, an
	// un-polled ring stays permanently full and the NEXT arm reports "pending"
	// forever, which reads exactly like "the pass never ran".
	const auto Poll = [](FVoxelMarchState::FTimingPair* Ring, float& OutMs)
	{
		float NewMs = -1.0f;
		for (int32 i = 0; i < FVoxelMarchState::kNumTimingPairs; ++i)
		{
			FVoxelMarchState::FTimingPair& Pair = Ring[i];
			if (!Pair.bInFlight)
			{
				continue;
			}
			uint64 BeginMicros = 0;
			uint64 EndMicros = 0;
			if (RHIGetRenderQueryResult(Pair.End.GetReference(), EndMicros, false) &&
			    RHIGetRenderQueryResult(Pair.Begin.GetReference(), BeginMicros, false))
			{
				NewMs = float(double(EndMicros - BeginMicros) / 1000.0);
				Pair.bInFlight = false;
			}
		}
		if (NewMs >= 0.0f)
		{
			OutMs = NewMs;
		}
	};

	float MarchMs = -1.0f;
	float EmitMs = -1.0f;
	float ScratchMs = -1.0f;
	// The bound producer's own bracket (voxel.March.Bound). Polled with the
	// other three and BEFORE the gate, for the same un-polled-ring reason.
	float BoundMs = -1.0f;
	Poll(State->MarchTiming, MarchMs);
	Poll(State->EmitTiming, EmitMs);
	Poll(State->ScratchTiming, ScratchMs);
	Poll(State->BoundTiming, BoundMs);
	if (MarchMs >= 0.0f || EmitMs >= 0.0f || ScratchMs >= 0.0f || BoundMs >= 0.0f)
	{
		FScopeLock Guard(&State->Lock);
		if (MarchMs >= 0.0f) { State->Stats.MarchGpuMs = MarchMs; }
		if (EmitMs >= 0.0f) { State->Stats.EmitGpuMs = EmitMs; }
		if (ScratchMs >= 0.0f) { State->Stats.ScratchGpuMs = ScratchMs; }
		if (BoundMs >= 0.0f) { State->Stats.BoundGpuMs = BoundMs; }
	}

	// The hit-tile count. Polled here, beside the timing poll and BEFORE the
	// gate, for the same reason: a ring left unpolled after the cvar goes to 0
	// stays full, and the next run then reports nothing while looking exactly
	// like a run that measured zero hit tiles.
	for (FVoxelMarchState::FTileReadback& Slot : State->TileRing)
	{
		if (!Slot.bInFlight || !Slot.Readback.IsValid() || !Slot.Readback->IsReady())
		{
			continue;
		}
		// Element 1 of FRHIDrawIndirectParameters is InstanceCount, which the
		// compact pass built with one InterlockedAdd per hit tile. Four uints are
		// copied and the second is read, rather than copying four bytes at an
		// offset, because a readback whose offset is wrong returns a plausible
		// number instead of an error.
		const uint32* Src = static_cast<const uint32*>(Slot.Readback->Lock(4 * sizeof(uint32)));
		if (Src != nullptr)
		{
			const uint32 Hit = Src[1];
			Slot.Readback->Unlock();
			FScopeLock Guard(&State->Lock);
			State->Stats.TilesHit = Hit;
		}
		Slot.bInFlight = false;
	}

	// The hole metric's ring, polled here BEFORE the gate for the reason the
	// rings above are: a slot left in flight when the cvar goes to 0 would
	// stay full forever and the next enable would silently measure nothing.
	// SUMMED into the window, not overwritten -- these are event counts, and
	// the frame that lands last is not more representative than the ones
	// before it (the tile count above is a level, so latest-wins is right
	// there and would be wrong here).
	for (FVoxelMarchState::FHoleReadback& Slot : State->HoleRing)
	{
		if (!Slot.bInFlight || !Slot.Readback.IsValid() || !Slot.Readback->IsReady())
		{
			continue;
		}
		const uint32* Src = static_cast<const uint32*>(
			Slot.Readback->Lock(uint32(VoxelMarchRayBoundWord::End) * sizeof(uint32)));
		if (Src != nullptr)
		{
			const uint32 Rays = Src[VoxelMarchHoleWord::Rays];
			const uint32 Hits = Src[VoxelMarchHoleWord::Hits];
			const uint32 Substituted = Src[VoxelMarchHoleWord::Substituted];
			const uint32 Uncovered = Src[VoxelMarchHoleWord::Uncovered];
			// The breakdown words, copied out BEFORE Unlock invalidates Src.
			// Read regardless of level (the buffer always has Count words),
			// folded in only for level-2 frames below.
			const uint32 UncoveredShell = Src[VoxelMarchHoleWord::UncoveredShell];
			// The fallthrough ladder. Read on EVERY frame, not only level-2 ones:
			// unlike the shell/level/reason breakdown these are written by the plain
			// kernel too, so folding them in below the level-2 gate would divide a
			// full-rate numerator by a level-2 denominator and under-report the rate.
			const uint32 FtConsidered = Src[VoxelMarchHoleWord::FallthroughConsidered];
			const uint32 FtTaken = Src[VoxelMarchHoleWord::FallthroughTaken];
			// The resident-Z bound's engagement trio. Read on EVERY frame for
			// the reason the ladder's pair is: the plain kernel writes them
			// too, so gating them behind the level-2 fold would report the
			// arm as inert on every level-1 leg -- the exact misreading these
			// counters exist to make impossible.
			const uint32 ZCutConsulted = Src[VoxelMarchHoleWord::ZCutConsulted];
			const uint32 ZCutSkipped = Src[VoxelMarchHoleWord::ZCutSkipped];
			const uint32 ZCutClipped = Src[VoxelMarchHoleWord::ZCutClipped];
			// The coarse occupancy level's trio. Read on EVERY frame for the
			// reason the Z bound's is: the cheap kernel writes them too, so
			// gating them behind the level-2 fold would report the arm inert on
			// every level-1 leg.
			const uint32 BlkConsulted = Src[VoxelMarchHoleWord::BlockConsulted];
			const uint32 BlkSkipped = Src[VoxelMarchHoleWord::BlockSkipped];
			const uint32 BlkCells = Src[VoxelMarchHoleWord::BlockCellsAvoided];
			// The sky licence's trio, read on every frame for the same reason.
			const uint32 BlkSkyLic = Src[VoxelMarchHoleWord::BlockSkyLicensed];
			const uint32 BlkSkyRunB = Src[VoxelMarchHoleWord::BlockSkyRunBlocks];
			const uint32 BlkSkyRuns = Src[VoxelMarchHoleWord::BlockSkyRuns];
			// The chunk-cell lookup census. Read on EVERY frame for the reason
			// the three groups above are: the cheap kernel writes them too, so
			// gating them behind the level-2 fold would report the census inert
			// on every level-1 leg.
			const uint32 CellsProbed = Src[VoxelMarchHoleWord::CellsProbed];
			const uint32 CellNonRes = Src[VoxelMarchHoleWord::CellNonResident];
			const uint32 CellIdxEmpty = Src[VoxelMarchHoleWord::CellIndexEmpty];
			const uint32 CellSlotRej = Src[VoxelMarchHoleWord::CellSlotReject];
			const uint32 CellStale = Src[VoxelMarchHoleWord::CellStale];
			const uint32 CellResEmpty = Src[VoxelMarchHoleWord::CellResidentEmpty];
			const uint32 CellBrickOOB = Src[VoxelMarchHoleWord::CellBrickOOB];
			const uint32 CellReal = Src[VoxelMarchHoleWord::CellReal];
			// The ray-bound census (Stage 0a), from the words appended after
			// the enum (VoxelMarchRayBoundWord). Read on EVERY frame for the
			// reason the cell census above is: the cheap kernel writes them
			// too, so gating them behind the level-2 fold would report the
			// census inert on every level-1 leg.
			const uint32 RbProbesUp = Src[VoxelMarchRayBoundWord::ProbesUp];
			const uint32 RbProbesDown = Src[VoxelMarchRayBoundWord::ProbesDown];
			const uint32 RbPreUp = Src[VoxelMarchRayBoundWord::PreUp];
			const uint32 RbPreDown = Src[VoxelMarchRayBoundWord::PreDown];
			const uint32 RbPostUp = Src[VoxelMarchRayBoundWord::PostUp];
			const uint32 RbPostDown = Src[VoxelMarchRayBoundWord::PostDown];
			const uint32 RbCapRays = Src[VoxelMarchRayBoundWord::CapRays];
			// The bound's engagement group (voxel.March.Bound), read on
			// EVERY frame for the reason every group above is: written only
			// by the bound permutation, structurally zero on all others, and
			// the drain's sibling line is what words a zero correctly.
			const uint32 BndConsulted = Src[VoxelMarchRayBoundWord::BoundConsulted];
			const uint32 BndSegSkipped = Src[VoxelMarchRayBoundWord::BoundSegmentsSkipped];
			const uint32 BndInRaised = Src[VoxelMarchRayBoundWord::BoundWalkInRaised];
			const uint32 BndOutLowered = Src[VoxelMarchRayBoundWord::BoundWalkOutLowered];
			// The height pyramid's engagement group. Read on EVERY frame for
			// the reason the three groups above are: the cheap kernel writes
			// them too, so gating them behind the level-2 fold would report the
			// arm inert on every level-1 leg -- the exact misreading these
			// counters exist to make impossible.
			const uint32 HgtConsulted = Src[VoxelMarchHoleWord::HeightConsulted];
			const uint32 HgtAdvanced = Src[VoxelMarchHoleWord::HeightAdvanced];
			const uint32 HgtEmpty = Src[VoxelMarchHoleWord::HeightEmpty];
			const uint32 HgtLeafCells = Src[VoxelMarchHoleWord::HeightLeafCells];
			const uint32 HgtSteps = Src[VoxelMarchHoleWord::HeightSteps];
			const uint32 HgtReentries = Src[VoxelMarchHoleWord::HeightReentries];
			const uint32 HgtMissed = Src[VoxelMarchHoleWord::HeightMissed];
			const uint32 HgtLate = Src[VoxelMarchHoleWord::HeightLate];
			const uint32 HgtLateMax = Src[VoxelMarchHoleWord::HeightLateMaxUU];
			uint32 HgtLateB[5];
			for (int32 B = 0; B < 5; ++B)
			{
				HgtLateB[B] = Src[int32(VoxelMarchHoleWord::HeightLateB0) + B];
			}
			uint32 ByLevel[VoxelMarchHoleWord::kNumLevels];
			uint32 ByReason[VoxelMarchHoleWord::kNumReasons];
			for (int32 L = 0; L < VoxelMarchHoleWord::kNumLevels; ++L)
			{
				ByLevel[L] = Src[int32(VoxelMarchHoleWord::UncLevelFirst) + L];
			}
			for (int32 R = 0; R < VoxelMarchHoleWord::kNumReasons; ++R)
			{
				ByReason[R] = Src[int32(VoxelMarchHoleWord::UncReasonFirst) + R];
			}
			Slot.Readback->Unlock();
			FScopeLock Guard(&State->Lock);
			State->HoleWindow.Rays += Rays;
			State->HoleWindow.Hits += Hits;
			State->HoleWindow.Substituted += Substituted;
			State->HoleWindow.Uncovered += Uncovered;
			State->HoleWindow.FallthroughConsidered += FtConsidered;
			State->HoleWindow.FallthroughTaken += FtTaken;
			State->HoleWindow.ZCutConsulted += ZCutConsulted;
			State->HoleWindow.ZCutSkipped += ZCutSkipped;
			State->HoleWindow.ZCutClipped += ZCutClipped;
			State->HoleWindow.BlockConsulted += BlkConsulted;
			State->HoleWindow.BlockSkipped += BlkSkipped;
			State->HoleWindow.BlockCellsAvoided += BlkCells;
			State->HoleWindow.BlockSkyLicensed += BlkSkyLic;
			State->HoleWindow.BlockSkyRunBlocks += BlkSkyRunB;
			State->HoleWindow.BlockSkyRuns += BlkSkyRuns;
			State->HoleWindow.CellsProbed += CellsProbed;
			State->HoleWindow.CellNonResident += CellNonRes;
			State->HoleWindow.CellIndexEmpty += CellIdxEmpty;
			State->HoleWindow.CellSlotReject += CellSlotRej;
			State->HoleWindow.CellStale += CellStale;
			State->HoleWindow.CellResidentEmpty += CellResEmpty;
			State->HoleWindow.CellBrickOOB += CellBrickOOB;
			State->HoleWindow.CellReal += CellReal;
			// The ray-bound census's window, under the SAME lock as the
			// HoleWindow fold, summed the same way (event counts across landed
			// frames), with its own Frames so its per-frame rates divide by
			// frames that actually carried census words.
			GVoxelMarchRayBoundWindow.ProbesUp += RbProbesUp;
			GVoxelMarchRayBoundWindow.ProbesDown += RbProbesDown;
			GVoxelMarchRayBoundWindow.PreUp += RbPreUp;
			GVoxelMarchRayBoundWindow.PreDown += RbPreDown;
			GVoxelMarchRayBoundWindow.PostUp += RbPostUp;
			GVoxelMarchRayBoundWindow.PostDown += RbPostDown;
			GVoxelMarchRayBoundWindow.CapRays += RbCapRays;
			GVoxelMarchRayBoundWindow.BoundConsulted += BndConsulted;
			GVoxelMarchRayBoundWindow.BoundSegmentsSkipped += BndSegSkipped;
			GVoxelMarchRayBoundWindow.BoundWalkInRaised += BndInRaised;
			GVoxelMarchRayBoundWindow.BoundWalkOutLowered += BndOutLowered;
			GVoxelMarchRayBoundWindow.Frames++;
			State->HoleWindow.HeightConsulted += HgtConsulted;
			State->HoleWindow.HeightAdvanced += HgtAdvanced;
			State->HoleWindow.HeightEmpty += HgtEmpty;
			State->HoleWindow.HeightLeafCells += HgtLeafCells;
			State->HoleWindow.HeightSteps += HgtSteps;
			State->HoleWindow.HeightReentries += HgtReentries;
			State->HoleWindow.HeightMissed += HgtMissed;
			State->HoleWindow.HeightLate += HgtLate;
			// MAX, NOT +=. The shader writes this with InterlockedMax within a
			// frame, so it is already a maximum; summing it across the window
			// would produce a number that grows with frame count and describes
			// nothing. This is the one word in the whole readback that does not
			// accumulate, and it is spelled differently on purpose.
			State->HoleWindow.HeightLateMaxUU =
				FMath::Max<uint64>(State->HoleWindow.HeightLateMaxUU, HgtLateMax);
			for (int32 B = 0; B < 5; ++B)
			{
				State->HoleWindow.HeightLateBucket[B] += HgtLateB[B];
			}
			State->HoleWindow.Frames++;
			if (Slot.ArmLevel >= 2)
			{
				// Only frames whose kernel actually ran the breakdown count
				// toward it. A level-1 frame's words are zeros by permutation,
				// not by measurement, and summing them would let the window
				// print a confident 0 for buckets nothing counted -- the
				// zeros-mistaken-for-measurements failure this project
				// retracted two readings over this week.
				for (int32 L = 0; L < VoxelMarchHoleWord::kNumLevels; ++L)
				{
					State->HoleWindow.UncoveredByLevel[L] += ByLevel[L];
				}
				for (int32 R = 0; R < VoxelMarchHoleWord::kNumReasons; ++R)
				{
					State->HoleWindow.UncoveredByReason[R] += ByReason[R];
				}
				// Level-2 only, folded here with the histograms and NOT beside
				// Uncovered above: a level-1 frame writes zero to this word by
				// permutation, and summing those in would let a mixed window
				// print a shell count that under-reports by however many
				// frames ran the cheap kernel.
				State->HoleWindow.UncoveredShell += UncoveredShell;
				State->HoleWindow.BreakdownFrames++;
			}
		}
		Slot.bInFlight = false;
	}

	// The source comparator's readback. One slot, on demand -- same reasoning as
	// the join probe: it is a diagnostic run against a settled pose, not a
	// per-frame instrument, and a ring would only add ways for it to report a
	// different frame than the one asked about.
	if (State->bSourceCompareInFlight && State->SourceCompareReadback.IsValid() &&
	    State->SourceCompareReadback->IsReady())
	{
		const uint32 Bytes = uint32(FVoxelMarchState::kSourceCompareWords) * sizeof(uint32);
		const uint32* Src = static_cast<const uint32*>(State->SourceCompareReadback->Lock(Bytes));
		if (Src != nullptr)
		{
			FVoxelMarchStats::FSourceCompare S;
			S.bValid = true;
			S.Rays = Src[0];
			S.AgreeMiss = Src[1];
			S.AgreeHit = Src[2];
			S.Ties = Src[3];
			S.BrickMissed = Src[4];
			S.BrickExtra = Src[5];
			S.Watermark = Src[6];
			S.Hard = Src[7];
			S.RefNoise = Src[8];
			S.OccNoise = Src[35];
			S.bOccValid = State->bSourceCompareOccValid;
			S.bArmRingsCpu = State->bSourceCompareArmRingsCpu;
			S.ViewRectSize = State->SourceCompareViewSize;
			S.Residency = Src[36];
			S.Uploads = Src[37];
			S.IndexHash = uint64(Src[38]) | (uint64(Src[39]) << 32);
			S.PoolLevel0 = Src[40];
			S.LostOnFace = Src[41];
			S.LostOffFace = Src[42];
			S.MovedOnFace = Src[43];
			S.MovedOffFace = Src[44];
			for (int32 d = 0; d < 3; ++d) { S.Descend[d] = Src[45 + d]; }
			for (int32 d = 0; d < 3; ++d) { S.MovedDelta[d] = Src[48 + d]; }
			S.BrickEntries = Src[51];
			S.ThinClips = Src[52];
			S.FlatHit = Src[53];
			S.HierHit = Src[54];
			S.HitThin = Src[55];
			S.DisHit  = Src[56];
			S.DisThin = Src[57];
			for (int32 i = 0; i < FVoxelMarchStats::FSourceCompare::kBandBins; ++i)
			{
				S.BandAll[i] = Src[58 + i];
				S.BandDis[i] = Src[70 + i];
			}
			S.ArbFlat    = Src[82];
			S.ArbHier    = Src[83];
			S.ArbTie     = Src[84];
			S.ArbNeither = Src[85];
			for (int32 i = 0; i < 6; ++i) { S.ArbFailA[i] = Src[86 + i]; }
			for (int32 i = 0; i < 6; ++i) { S.ArbFailB[i] = Src[92 + i]; }
			for (int32 i = 0; i < 7; ++i) { S.ArbFailAReason[i] = Src[98 + i]; }
			for (int32 i = 0; i < FVoxelMarchStats::FSourceCompare::kBandBins; ++i)
			{
				S.ArbFailBand[i] = Src[105 + i];
			}
			S.ArbTotal = Src[117];
			S.ArbFlatOnly = Src[118];
			S.ArbHierOnly = Src[119];
			S.SegHiMaxOverUU = *reinterpret_cast<const float*>(&Src[120]);
			S.RingDiscordant = Src[121];
			for (int32 L = 0; L < FVoxelMarchStats::FSourceCompare::kRingLevels; ++L)
			{
				S.FlatOnlyL[L]  = Src[122 + L];
				S.HierOnlyL[L]  = Src[128 + L];
				S.HitAtLevel[L] = Src[134 + L];
			}
			S.BudgetFlat = Src[140];
			S.BudgetHier = Src[141];
			S.MaxStepsFlat = Src[142];
			for (int32 i = 0; i < 3; ++i) { S.EarlyOut[i] = Src[143 + i]; }
			for (int32 L = 0; L < FVoxelMarchStats::FSourceCompare::kRingLevels; ++L)
			{
				S.EnteredLevel[L] = Src[146 + L];
			}
			S.Sky = Src[152];
			S.FrameNumber = Src[153];
			S.QuietFrames = Src[154];
			S.GpuResidency = Src[36];
			S.GpuUploads = Src[37];
			S.ArmStamp = Src[155];
			S.Unattributed = Src[156];
			for (int32 i = 0; i < 8; ++i) { S.DisTileMask[i] = Src[157 + i]; }
			{
				// minX/minY came back inverted (stored as W-x via InterlockedMax
				// because the buffer clears to zero). Undo it here, in the one
				// place that knows the convention.
				const int32 W = FMath::Max(S.ViewRectSize.X, 1);
				const int32 Hh = FMath::Max(S.ViewRectSize.Y, 1);
				S.DisBoxMin = FIntPoint(W - int32(Src[165]), Hh - int32(Src[166]));
				S.DisBoxMax = FIntPoint(int32(Src[167]), int32(Src[168]));
			}
			S.Claimed = Src[9];
			for (int32 r = 0; r < 7; ++r)
			{
				S.Reasons[r] = Src[10 + r];
			}
			S.StepsOcc = Src[17];
			S.StepsFlat = Src[18];
			S.StepsHier = Src[19];
			S.HierVsFlat = Src[20];
			S.HvfLost = Src[21];
			S.HvfGained = Src[22];
			S.HvfMoved = Src[23];
			for (int32 i = 0; i < 4; ++i) { S.LostTerm[i] = Src[24 + i]; }
			for (int32 i = 0; i < 7; ++i) { S.LostProbe[i] = Src[28 + i]; }
			{
				const int32 HvfBase = 169 + 6 * 11;   // mirrors VOXEL_MARCH_VSRC_HVF_CLAIMED
				S.HvfClaimed = Src[HvfBase];
				S.HvfSampleCount = int32(FMath::Min<uint32>(
					S.HvfClaimed, uint32(FVoxelMarchStats::FSourceCompare::kMaxHvfSamples)));
				for (int32 i = 0; i < S.HvfSampleCount; ++i)
				{
					const uint32* B = Src + HvfBase + 1 + i * 24;
					S.HvfSamples[i].Pixel = FIntPoint(int32(B[0]), int32(B[1]));
					S.HvfSamples[i].FlatVoxel = FIntVector(int32(B[2]), int32(B[3]), int32(B[4]));
					S.HvfSamples[i].HierVoxel = FIntVector(int32(B[5]), int32(B[6]), int32(B[7]));
					S.HvfSamples[i].FlatT = *reinterpret_cast<const float*>(&B[8]);
					S.HvfSamples[i].HierT = *reinterpret_cast<const float*>(&B[9]);
					S.HvfSamples[i].Class = B[10];
					S.HvfSamples[i].TermReason = B[11];
					S.HvfSamples[i].HierSteps = B[12];
					S.HvfSamples[i].ProbeReason = B[13];
					S.HvfSamples[i].LastChunk = FIntVector(int32(B[14]), int32(B[15]), int32(B[16]));
					S.HvfSamples[i].LastBrick = int32(B[17]);
					S.HvfSamples[i].LastFineBrick = int32(B[18]);
					S.HvfSamples[i].LastFineT = *reinterpret_cast<const float*>(&B[19]);
					S.HvfSamples[i].FineWalks = B[20];
					S.HvfSamples[i].FlatFace = B[21];
					S.HvfSamples[i].FlatCellInBrick = B[22];
					S.HvfSamples[i].DescendVerdict = B[23];
				}
			}
			S.SampleCount = int32(FMath::Min<uint32>(
				S.Claimed, uint32(FVoxelMarchStats::FSourceCompare::kMaxSamples)));
			for (int32 i = 0; i < S.SampleCount; ++i)
			{
				const uint32* B = Src + 169 + i * 11;   // mirrors VOXEL_MARCH_VSRC_HEADER
				S.Samples[i].Pixel = FIntPoint(int32(B[0]), int32(B[1]));
				S.Samples[i].OccVoxel = FIntVector(int32(B[2]), int32(B[3]), int32(B[4]));
				S.Samples[i].BrickVoxel = FIntVector(int32(B[5]), int32(B[6]), int32(B[7]));
				S.Samples[i].OccT = *reinterpret_cast<const float*>(&B[8]);
				S.Samples[i].BrickSteps = B[9];
				S.Samples[i].Reason = B[10];
			}
			State->SourceCompareReadback->Unlock();
			FScopeLock Guard(&State->Lock);
			State->Stats.SourceCompare = S;
		}
		State->bSourceCompareInFlight = false;
	}

	// The join probe's readback. One slot, on demand.
	if (State->bIndexProbeInFlight && State->IndexProbeReadback.IsValid() &&
	    State->IndexProbeReadback->IsReady())
	{
		const uint32 Bytes = uint32(FVoxelMarchState::kIndexProbeWords) * sizeof(uint32);
		const uint32* Src = static_cast<const uint32*>(State->IndexProbeReadback->Lock(Bytes));
		if (Src != nullptr)
		{
			FVoxelMarchStats::FIndexProbe P;
			P.bValid = true;
			P.Probed = Src[0];
			P.Resident = Src[1];
			P.SlotOutOfRange = Src[2];
			P.OriginMismatch = Src[3];
			P.LevelMismatch = Src[4];
			P.AnySolidClear = Src[5];
			P.BrickBaseOutOfRange = Src[6];
			P.Valid = Src[7];
			P.L1Any = Src[8];
			P.L1Zero = Src[9];
			P.Claimed = Src[10];
			P.PoolSolidCells = Src[11];
			P.OccSolidVoxels = Src[12];
			P.KindAir = Src[13];
			P.KindSolid = Src[14];
			P.KindMixed = Src[15];
			P.SampleCount = int32(FMath::Min<uint32>(
				P.Claimed, uint32(FVoxelMarchStats::FIndexProbe::kMaxSamples)));
			for (int32 i = 0; i < P.SampleCount; ++i)
			{
				const uint32* S = Src + 16 + i * 8;
				P.Samples[i].Want = FIntVector(int32(S[0]), int32(S[1]), int32(S[2]));
				P.Samples[i].Got = FIntVector(int32(S[3]), int32(S[4]), int32(S[5]));
				P.Samples[i].Slot = S[6];
				P.Samples[i].LevelAndFlags = S[7];
			}
			State->IndexProbeReadback->Unlock();
			FScopeLock Guard(&State->Lock);
			State->Stats.IndexProbe = P;
		}
		State->bIndexProbeInFlight = false;
	}

	// The depth gate's readback. Same discipline: polled before the gate so a
	// ring left full by a cvar flip cannot make the next run look like it
	// produced nothing.
	for (FVoxelMarchState::FVerifyReadback& Slot : State->VerifyRing)
	{
		if (!Slot.bInFlight || !Slot.Readback.IsValid() || !Slot.Readback->IsReady())
		{
			continue;
		}
		const uint32 Bytes = uint32(FVoxelMarchState::kVerifyWords) * sizeof(uint32);
		const uint32* Src = static_cast<const uint32*>(Slot.Readback->Lock(Bytes));
		if (Src != nullptr)
		{
			FVoxelMarchStats::FDepthGate G;
			G.bValid = true;
			G.Generation = Slot.Generation;
			G.Compared = Src[0];
			G.Disagree = Src[1];
			G.DisagreeEdge = Src[2];
			G.DisagreeInterior = Src[3];
			G.MaxDeltaMilliVoxels = Src[4];
			G.HitOnSky = Src[5];
			G.Miss = Src[6];
			G.EdgePixels = Src[7];
			for (int32 b = 0; b < 6; ++b)
			{
				G.Buckets[b] = Src[8 + b];
			}
			G.InteriorMaxMilliVoxels = Src[14];
			for (int32 b = 0; b < 6; ++b)
			{
				G.InteriorBuckets[b] = Src[15 + b];
			}
			G.RefSelfDisagree = Src[21];
			G.RefSelfDisagreeInterior = Src[22];
			G.RefSelfMaxMilliVoxels = Src[23];
			// Every read of Src is above this line: the unlock invalidates the
			// mapping and a decode that ran after it would read freed memory
			// while producing entirely ordinary-looking numbers.
			Slot.Readback->Unlock();

			// ---- accumulate into the open window ----------------------------
			//
			// The GPU buffer is cleared every frame, so each readback is ONE
			// frame -- and one arbitrary frame is not a comparable unit. The
			// ring hands back whichever frame had a slot free, so two reads of a
			// moving scene were two different frames and a drifting result could
			// not be told apart from a settled one. Summing a fixed number of
			// them gives a sample of stated size that can converge.
			//
			// SUMS FOR COUNTS, MAX FOR MAXES. Averaging the maxes would hide the
			// single worst pixel, which is the one the gate exists to find.
			FVoxelMarchStats::FDepthGate& A = State->VerifyAccum;
			A.bValid = true;
			A.Generation = G.Generation;
			A.Compared += G.Compared;
			A.Disagree += G.Disagree;
			A.DisagreeEdge += G.DisagreeEdge;
			A.DisagreeInterior += G.DisagreeInterior;
			A.EdgePixels += G.EdgePixels;
			A.HitOnSky += G.HitOnSky;
			A.Miss += G.Miss;
			A.RefSelfDisagree += G.RefSelfDisagree;
			A.RefSelfDisagreeInterior += G.RefSelfDisagreeInterior;
			A.MaxDeltaMilliVoxels = FMath::Max(A.MaxDeltaMilliVoxels, G.MaxDeltaMilliVoxels);
			A.InteriorMaxMilliVoxels =
				FMath::Max(A.InteriorMaxMilliVoxels, G.InteriorMaxMilliVoxels);
			A.RefSelfMaxMilliVoxels =
				FMath::Max(A.RefSelfMaxMilliVoxels, G.RefSelfMaxMilliVoxels);
			for (int32 b = 0; b < 6; ++b)
			{
				A.Buckets[b] += G.Buckets[b];
				A.InteriorBuckets[b] += G.InteriorBuckets[b];
			}
			State->VerifyAccumFrames++;
			A.SampleFrames = uint32(State->VerifyAccumFrames);

			const int32 Window = FMath::Clamp(
				CVarVoxelMarchVerifyDepth.GetValueOnRenderThread(), 1, 4096);
			if (State->VerifyAccumFrames >= Window)
			{
				{
					FScopeLock Guard(&State->Lock);
					State->Stats.DepthGate = A;
					State->VerifyAccum = FVoxelMarchStats::FDepthGate();
					State->VerifyAccumFrames = 0;
				}

				// SAY SO WHEN THE WINDOW CLOSES, RATHER THAN WAITING TO BE ASKED.
				//
				// This is the project's most important correctness gate -- it
				// compares marched depth against raster depth in the same frame,
				// so the quad path IS its reference and it can never run once the
				// renderer is switched over. Until now the ONLY way to see a
				// result was to invoke voxel.March.Stats, an FAutoConsoleCommand
				// (:478) that prints once per invocation.
				//
				// MEASURED COST OF THAT, 2026-08-22: a leg was run with
				// VerifyDepth 64 and voxel.March.Stats in -ExecCmds. -ExecCmds
				// fires at engine init, so the one stats block printed at FRAME 0
				// -- "frames=0 emitFrames=0 indexEntries=0", "depth gate: NOT RUN"
				// -- while the gate then ran correctly for 871 frames and closed
				// its window about thirteen times. A perfectly healthy gate
				// produced a log that said NOT RUN, and the run was scrapped.
				//
				// A gate whose result exists only if an operator remembers to
				// schedule a console command inside the flight window is a gate
				// that will be missed, and the failure looks exactly like the
				// feature being broken. One line here removes that entirely; the
				// console command stays for on-demand reads.
				// INTERIOR against the CONTROL, because those are the two numbers
				// the verdict is actually made of -- a raw disagreement total is
				// dominated by silhouettes, where the two renderers are entitled
				// to disagree. See FDepthGate's own field comments.
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("Voxel march depth gate [%u frames]: compared=%u interiorDisagree=%u "
				            "control(refSelfInterior)=%u interiorMax=%.3f vox miss=%u hitOnSky=%u "
				            "-- window closed. Read voxel.March.Stats for the verdict and its "
				            "ingredients; interior ABOVE control is a hard result, below it is "
				            "consistent with sampling noise and is NOT proof."),
				       A.SampleFrames, A.Compared, A.DisagreeInterior, A.RefSelfDisagreeInterior,
				       double(A.InteriorMaxMilliVoxels) / 1000.0, A.Miss, A.HitOnSky);
			}
		}
		Slot.bInFlight = false;
	}
}

namespace
{
	// Grab a free pair and open the bracket. Returns the pair so the caller can
	// close it; null means no pair was free or the RHI has no timestamps, and
	// the pass still runs -- it just goes unmeasured this frame.
	FVoxelMarchState::FTimingPair* OpenBracket(FRDGBuilder& GraphBuilder,
	                                           FVoxelMarchState::FTimingPair* Ring,
	                                           const TCHAR* Name)
	{
		if (!GSupportsTimestampRenderQueries)
		{
			return nullptr;
		}
		FVoxelMarchState::FTimingPair* Timing = nullptr;
		for (int32 i = 0; i < FVoxelMarchState::kNumTimingPairs; ++i)
		{
			if (!Ring[i].bInFlight)
			{
				Timing = &Ring[i];
				break;
			}
		}
		if (Timing == nullptr)
		{
			return nullptr;
		}
		if (!Timing->Begin.IsValid())
		{
			Timing->Begin = RHICreateRenderQuery(RQT_AbsoluteTime);
			Timing->End = RHICreateRenderQuery(RQT_AbsoluteTime);
		}
		FRHIRenderQuery* Query = Timing->Begin.GetReference();
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelMarch.TimeBegin(%s)", Name),
		                     ERDGPassFlags::NeverCull,
		                     [Query](FRHICommandListImmediate& RHICmdList)
		                     {
			                     RHICmdList.EndRenderQuery(Query);
		                     });
		return Timing;
	}

	void CloseBracket(FRDGBuilder& GraphBuilder, FVoxelMarchState::FTimingPair* Timing,
	                  const TCHAR* Name)
	{
		if (Timing == nullptr)
		{
			return;
		}
		FRHIRenderQuery* Query = Timing->End.GetReference();
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelMarch.TimeEnd(%s)", Name),
		                     ERDGPassFlags::NeverCull,
		                     [Query](FRHICommandListImmediate& RHICmdList)
		                     {
			                     RHICmdList.EndRenderQuery(Query);
		                     });
		Timing->bInFlight = true;
	}
}

// ---------------------------------------------------------------------------
// ANCHOR A -- the render-frame split's frame boundary
// ---------------------------------------------------------------------------
//
// This hook does NO RENDERING WORK AND MUST NOT ACQUIRE ONE. It is the first
// render-thread hook the engine calls (SceneRendering.cpp:4299) and its only
// job is to close the previous frame's sample and open this one. Deliberately
// BEFORE any arm check: a frame in which the marcher declines is still a frame
// whose render thread did 9-18 ms of something, and that is precisely the frame
// the split exists to describe.
void FVoxelMarchRenderExtension::PreRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	// No scope here on purpose. Anchor A IS the start of the frame, so a bucket
	// scope wrapping the call that takes the anchor would either measure nothing
	// or measure across the anchor -- and a sub-bucket that straddles its own
	// frame boundary is how setupOther starts printing negative for a reason
	// that has nothing to do with the split being wrong.
	VoxelRenderFrame::Touch(GraphBuilder);
}

// ANCHOR B. Last render-thread hook of the scene renderer
// (SceneRendering.cpp:4956). Everything after it on this thread is RDG Execute
// and then the tail.
void FVoxelMarchRenderExtension::PostRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	// Symmetrically unscoped: anchor B IS the end of setup. mFam therefore reads
	// 0.000 by construction and is kept in the bucket list only so a future hook
	// added here has a named place to land instead of silently inflating
	// setupOther. Its DEAD READING is that zero, and it is stated here rather
	// than left for a reader to discover.
	VoxelRenderFrame::NoteSetupEnd();
}

// ---------------------------------------------------------------------------
// HOOK 1 -- stash
// ---------------------------------------------------------------------------
void FVoxelMarchRenderExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder,
                                                            FSceneView& InView)
{
	// ANCHOR FIRST, THEN THE SCOPE. The anchor is taken here as well as in the
	// family hook because extension iteration order is not ours to control: if
	// another extension's PreRenderViewFamily ran first the frame is already
	// open and this is a no-op, and if the family hook ever stopped being called
	// this keeps the split alive instead of silently emitting nothing. Touch()
	// is idempotent within a frame. Taking it before the scope opens matters:
	// a scope that started before its own frame did would bill this frame for
	// the previous one's tail.
	VoxelRenderFrame::Touch(GraphBuilder);
	VoxelRenderFrame::NoteView(InView.ViewMatrices.GetViewOrigin());
	VOXEL_RENDER_FRAME_SCOPE(MarchView);

	RetireTimingQueries();

	const FVoxelMarchArm Arm = VoxelMarchGetArm();
	if (Arm.Mode == 0)
	{
		return;
	}

	TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe> Volume;
	{
		FScopeLock Guard(&State->Lock);
		Volume = State->Volume;
	}
	const FVoxelMarchArm ArmForVolume = VoxelMarchGetArm();
	if (!Volume.IsValid() && VoxelMarchNeedsOccupancyVolume(ArmForVolume))
	{
		FScopeLock Guard(&State->Lock);
		State->Stats.DeclinedNoVolume++;
		return;
	}

	// Drop everything from a previous frame. A view that is culled between this
	// hook and the emit never tells anyone it went away, so the stash is aged by
	// frame number rather than unregistered.
	const uint32 FrameNumber = GFrameNumberRenderThread;
	Views.RemoveAll([FrameNumber](const FViewMarch& E) { return E.FrameNumber != FrameNumber; });

	const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(InView);
	if (ViewRect.Area() <= 0)
	{
		return;
	}

	// THE MARCHER RUNS ON THE PLAYER'S VIEW ONLY, AND UNTIL 2026-08-25 IT SAID SO
	// NOWHERE -- positive area was the whole test, so any view the engine handed
	// this hook was marched.
	//
	// That is latent, not theoretical. It crashed a leg. With
	// voxel.Sky.RealTimeCapture 0 the sky light falls back to a ONE-SHOT cubemap
	// capture, which renders six 128x128 cube faces through this same hook; the
	// marcher joined them, and registered resources against a graph that is not
	// the one it built them for:
	//
	//   Assertion failed: ResourceMap.Contains(Resource)
	//   Resource ... registered with pass VoxelMarch.March(128x128, budget 3328)
	//   is not part of the graph and is likely a dangling pointer
	//
	// The steady-state default hides it: with real-time capture ON the engine
	// re-uses cached sky data, so a control leg shows exactly ONE March pass per
	// frame at 1552x873 and views/frame=1.00, and zero 128x128 passes. The
	// nocap leg shows two. So this was reachable by anything that triggers a
	// non-real-time capture -- a scene capture component, a planar reflection,
	// a cubemap recapture -- and the sky arm is merely what found it first.
	//
	// Marching a 128x128 cube face is also pure waste even where it does not
	// crash: the capture wants sky and distant atmosphere, and the marcher draws
	// near-field terrain into a probe nobody reads terrain out of.
	//
	// Declined BY REASON, per this struct's own "not one flag" rule, so a leg
	// can tell "no capture happened" from "captures happened and were skipped".
	if (InView.bIsSceneCapture || InView.bIsReflectionCapture || InView.bIsPlanarReflection)
	{
		FScopeLock Guard(&State->Lock);
		State->Stats.DeclinedNonPrimary++;
		return;
	}

	const FViewMatrices& VM = InView.ViewMatrices;
	const FMatrix44f ViewToClip = FMatrix44f(VM.GetViewToClip());
	const float ProjXX = ViewToClip.M[0][0];
	const float ProjYY = ViewToClip.M[1][1];
	if (ProjXX == 0.0f || ProjYY == 0.0f)
	{
		return; // degenerate projection; no ray to build
	}

	FViewMarch Entry;
	Entry.ViewKey = &InView;
	Entry.FrameNumber = FrameNumber;
	Entry.ViewRect = ViewRect;
	Entry.ViewToTranslatedWorld = FMatrix44f(VM.GetInvTranslatedViewMatrix());
	Entry.InvProjDiag = FVector2f(1.0f / ProjXX, 1.0f / ProjYY);

	// THE FRAME'S TAA/TSR PHASE, resolved once (voxel.March.TAAJitter).
	//
	// VM.GetTemporalAAJitter() is the value UE added to ViewToClip.M[2][0] and
	// M[2][1] -- already in NDC, already sign-flipped in Y to clip convention
	// (SceneVisibility.cpp:5450 passes SampleY * -2 / Height). So it subtracts
	// straight off the shader's Ndc, which has itself already been flipped to
	// clip convention on the line above the ray. No scaling, no second negate.
	//
	// Read ONCE, not per bind site: the march, the emit and the verify gate all
	// copy this Entry, so they provably share one phase.
	// TemporalAAJitter IS DELIBERATELY NOT RESOLVED HERE, AND THIS IS THE WHOLE
	// TRAP. THIS HOOK RUNS BEFORE THE FRAME HAS A JITTER AT ALL.
	//
	// PreRenderView_RenderThread is called from FSceneRenderer::OnRenderBegin
	// (SceneRendering.cpp:4303), which the deferred renderer runs at
	// DeferredShadingRenderer.cpp:1892 -- deliberately early, so that an
	// extension can still move the camera ("This must be done after
	// ViewExtension->PreRenderView_RenderThread performs any updates to the
	// camera position", SceneRendering.cpp:4317). The jitter is added ~230 lines
	// later, in BeginInitViews (:2123) -> PreVisibilityFrameSetup
	// (SceneVisibility.cpp:5918 -> 5911 -> 4986) ->
	// HackAddTemporalAAProjectionJitter (:5450).
	//
	// So VM.GetTemporalAAJitter() is EXACTLY (0,0) here on every frame, in every
	// configuration, however TSR is set up. MEASURED 2026-08-26: resolving it at
	// this hook produced (0.000000, 0.000000) for 120+ consecutive frames on a
	// free-running capture with TSR upscaling 1552x873 -> 2560x1440, and the arm
	// was SILENTLY INERT while every switch read as armed.
	//
	// It is resolved in PreRenderBasePass_RenderThread instead, which runs after
	// InitViews. Nothing else in this stash has the problem: the jitter never
	// touches the projection DIAGONAL that InvProjDiag is taken from, nor the
	// view matrix, so those are the same before and after.
	Entry.TemporalAAJitter = FVector2f::ZeroVector;

	// THE PRECISION SEAM, and it is the same one the spike documents: both
	// operands are world UU at planet scale (tens of km, past where float32 UU
	// loses sub-voxel precision) and their DIFFERENCE is bounded by the volume's
	// 5,120 UU. So the subtraction happens in double and the narrowing after it.
	// Doing it the other way round puts the marched terrain a few metres from
	// the drawn terrain, which reads as a worldgen bug and not as a renderer one.
	// SOURCE 0 ONLY. Under source 1 this is dead -- the frame block below
	// recomputes it camera-anchored -- and with no volume there is nothing to
	// subtract. Zero rather than a stale value so a leak reads as the origin.
	Entry.RayOriginLocalUU = Volume.IsValid()
		? FVector3f(VM.GetViewOrigin() - Volume->GetOriginUU())
		: FVector3f::ZeroVector;
	// In DOUBLE, then floored. The camera is at planet scale and a float divide
	// here would quantise coarser than a voxel long before it reached the frame.
	{
		const FVector CamUU = VM.GetViewOrigin();
		Entry.ViewOriginUU = CamUU;
		Entry.CameraVoxel = FIntVector(int32(FMath::FloorToDouble(CamUU.X / 10.0)),
		                               int32(FMath::FloorToDouble(CamUU.Y / 10.0)),
		                               int32(FMath::FloorToDouble(CamUU.Z / 10.0)));
	}

	// THE CONE SLOPE -- the quantity the whole LOD rule is written in, and the
	// reason this renderer contains no ddx/ddy at all (ADR-0008: hardware
	// derivatives are discontinuous across voxel edges, so a derivative-selected
	// mip flickers along every silhouette).
	//
	// DERIVATION, two lines. At view-space depth z the visible half-height in
	// world UU is z / ProjYY, spread over ViewRect.Height()/2 pixels, so one
	// pixel's half-width is z * 2 / (ProjYY * Height). The same for x. We take
	// the LARGER of the two -- for square pixels they are equal, and where they
	// are not, the larger is the conservative choice (a coarser voxel, never a
	// finer one than the footprint justifies).
	//
	// AND IT IS EXPRESSED PER UNIT OF t, NOT PER UNIT OF z. t is distance along
	// the ray and z is its forward component, so t >= z and using t overstates
	// the footprint slightly toward the screen edges. That is again the
	// conservative direction, and it costs one cosine of accuracy at the corner
	// of a 90-degree FOV in exchange for not carrying a per-pixel correction
	// through every level test.
	const float SlopeX = 2.0f / (ProjXX * float(ViewRect.Width()));
	const float SlopeY = 2.0f / (ProjYY * float(ViewRect.Height()));
	Entry.PixelConeSlope = FMath::Max(SlopeX, SlopeY);

	Views.Add(Entry);
}

// ---------------------------------------------------------------------------
// HOOK 2 -- classify + march
// ---------------------------------------------------------------------------
void FVoxelMarchRenderExtension::PreRenderBasePass_RenderThread(FRDGBuilder& GraphBuilder,
                                                                bool bDepthBufferIsPopulated)
{
	VOXEL_RENDER_FRAME_SCOPE(MarchBase);
	// MUTATION ARMS 1 and 3. See VoxelRenderFrame.h: arm 1 burns CPU here and
	// must appear in mBase; arm 3 BLOCKS here and must appear in sveBlocked and
	// renderWait while leaving mBase busy and renderBusy unchanged. Arm 3 is the
	// one that can come out red and invalidate the whole file, which is why it
	// is the arm to run first.
	VoxelRenderFrame::MutateHere(1);
	VoxelRenderFrame::MutateHere(3);
	if (VoxelRenderFrame::MutateArm() == 4)
	{
		// MUTATION ARM 4: burn inside an RDG pass lambda, which runs during
		// Execute and must therefore land in executeMs, not setupMs. Gated on
		// the arm being SELECTED, not on the instrument being armed: an
		// ordinary -VoxelRenderFrame=1 leg must add no pass, or the measured
		// build and the control differ by one and the split is measuring
		// itself.
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelRenderFrame.Mutate4"),
		                     ERDGPassFlags::NeverCull,
		                     [](FRHICommandListImmediate&)
		                     {
			                     VoxelRenderFrame::MutateHere(4);
		                     });
	}

	const FVoxelMarchArm Arm = VoxelMarchGetArm();
	if (Arm.Mode == 0)
	{
		return;
	}
	if (Views.Num() == 0)
	{
		FScopeLock Guard(&State->Lock);
		State->Stats.DeclinedNoView++;
		return;
	}

	// ---- THE FRAME'S TAA/TSR PHASE, RESOLVED WHERE IT EXISTS ---------------
	//
	// HERE AND NOT IN THE STASH HOOK. PreRenderView_RenderThread runs from
	// OnRenderBegin (DeferredShadingRenderer.cpp:1892), and the jitter is not
	// added to the projection until BeginInitViews (:2123) ->
	// PreVisibilityFrameSetup -> HackAddTemporalAAProjectionJitter
	// (SceneVisibility.cpp:5450). Read at the stash it is always exactly (0,0);
	// read HERE, inside the base pass, it is the real per-frame offset.
	//
	// ONE RESOLVE FEEDS ALL THREE CONSUMERS. This writes into the stashed
	// FViewMarch, and PostRenderBasePassDeferred_RenderThread recovers the SAME
	// entry through FindView(&InView) later in the same frame -- so the march,
	// the emit's ray reconstruction and the verify gate provably share one
	// phase, which is the property the whole FVoxelMarchViewParameters struct
	// exists to guarantee. Placed above the volume guard so a frame that
	// declines to march still leaves a truthful value behind rather than a
	// stale one.
	//
	// ViewKey is the FSceneView this entry was stashed from, this frame; Views
	// is pruned by frame number at the top of the stash hook, so it cannot be a
	// pointer from a previous frame.
	{
		const bool bArmed = CVarVoxelMarchTAAJitter.GetValueOnRenderThread() != 0;
		FVector2D FrameJitter = FVector2D::ZeroVector;
		// WHAT THE SHADER WILL ACTUALLY GET, read back OUT of the stash entry
		// after the write rather than re-derived from bArmed and FrameJitter.
		// The house failure is a join COMPUTED instead of CHECKED: "arm is on
		// and the engine phase is non-zero, therefore the shader is jittered"
		// is exactly that inference, and it stays true even if the entry the
		// bind sites read is a different one. This is the value the three bind
		// sites copy into MarchTemporalAAJitter, so a stuck or zero reading
		// here is a stuck or zero UNIFORM.
		FVector2f ResolvedJitter = FVector2f::ZeroVector;
		FIntRect JitterRect;
		for (FViewMarch& E : Views)
		{
			// THE FRAME GUARD IS NOT OPTIONAL AND A NULL CHECK DOES NOT STAND IN
			// FOR IT. ViewKey is a RAW POINTER to an FSceneView owned by the
			// renderer (stashed at Entry.ViewKey = &InView). A viewport resize --
			// F11, a resolution change, a PIE window rebuild -- destroys those
			// views and builds new ones, and a stale FViewMarch from the previous
			// frame survives in Views with a pointer that is DANGLING, NOT NULL.
			// Dereferencing it is EXCEPTION_ACCESS_VIOLATION, which is exactly
			// what F11 produced on 2026-08-26 with this loop guarded on null
			// alone. The loop at the bottom of this function has always carried
			// this test; these two jitter loops were added without it.
			if (E.ViewKey == nullptr || E.FrameNumber != GFrameNumberRenderThread)
			{
				continue;
			}
			const FVector2D J = E.ViewKey->ViewMatrices.GetTemporalAAJitter();
			E.TemporalAAJitter =
			    bArmed ? FVector2f(float(J.X), float(J.Y)) : FVector2f::ZeroVector;
			FrameJitter = J;
			ResolvedJitter = E.TemporalAAJitter; // read back, not re-derived
			JitterRect = E.ViewRect;
		}

		// ---- THE SIGN, CHECKED AGAINST THE ENGINE'S OWN MATRICES ----------
		//
		// ONE SHOT, AND IT CAN FAIL. Getting the sign backwards would not look
		// like a bug: it would DOUBLE the misalignment instead of removing it,
		// and every other reading in this investigation -- the arm engages, the
		// effect scales with |J|, the image changes far above the noise floor --
		// would come out exactly the same. Image-space displacement estimators
		// were tried first and could not settle it: phase correlation and a
		// best-fit translation DISAGREED on sign for one of the two pinned
		// phases, because the two images differ by more than a translation
		// (different sub-pixel samples of an aliased signal, then reconstructed).
		//
		// So this asks the engine instead, using its own no-AA matrix rather
		// than anything re-derived here:
		//
		//   A. the projection DIAGONAL must be identical with and without the
		//      jitter -- that is the whole defect, stated as an assertion:
		//      MarchInvProjDiag physically cannot see the jitter.
		//   B. the jitter must be exactly the THIRD-ROW delta between the
		//      jittered and un-jittered matrices, i.e.
		//      ViewToClip.M[2][0] - NoAA.M[2][0] == GetTemporalAAJitter().X.
		//
		// If both hold then, with UE's row-vector convention and M[2][3] == 1,
		// ndc = d * M00 + J and therefore d = (ndc - J) / M00 -- which is
		// arithmetic, not judgement, and is exactly what the shader now does.
		if (!GVoxelMarchTAAJitterSignChecked && !FrameJitter.IsNearlyZero())
		{
			for (const FViewMarch& E : Views)
			{
				// Same guard, same reason as the resolve loop above: a stale
				// entry's ViewKey is dangling rather than null after a viewport
				// rebuild, and ComputeProjectionNoAAMatrix() below dereferences
				// it twice.
				if (E.ViewKey == nullptr || E.FrameNumber != GFrameNumberRenderThread)
				{
					continue;
				}
				GVoxelMarchTAAJitterSignChecked = true;
				const FViewMatrices& CheckVM = E.ViewKey->ViewMatrices;
				const FMatrix Jittered = CheckVM.GetViewToClip();
				const FMatrix NoAA = CheckVM.ComputeProjectionNoAAMatrix();
				const double DiagDX = Jittered.M[0][0] - NoAA.M[0][0];
				const double DiagDY = Jittered.M[1][1] - NoAA.M[1][1];
				const double RowDX = Jittered.M[2][0] - NoAA.M[2][0];
				const double RowDY = Jittered.M[2][1] - NoAA.M[2][1];
				const FVector2D J = CheckVM.GetTemporalAAJitter();
				const bool bDiagClean = FMath::IsNearlyZero(DiagDX, 1e-12) &&
				                        FMath::IsNearlyZero(DiagDY, 1e-12);
				const bool bRowIsJitter = FMath::IsNearlyEqual(RowDX, J.X, 1e-12) &&
				                          FMath::IsNearlyEqual(RowDY, J.Y, 1e-12);
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("[voxel-march] taajitter signcheck: diag delta (%.3e, %.3e) -> %s | "
				            "row2 delta (%+.9f, %+.9f) vs jitter (%+.9f, %+.9f) -> %s | %s"),
				       DiagDX, DiagDY,
				       bDiagClean ? TEXT("DIAGONAL IS JITTER-BLIND (the defect, asserted)")
				                  : TEXT("*** DIAGONAL MOVED -- the whole diagnosis is wrong ***"),
				       RowDX, RowDY, J.X, J.Y,
				       bRowIsJitter ? TEXT("JITTER LIVES IN ROW 2, EXACTLY")
				                    : TEXT("*** ROW 2 DELTA IS NOT THE JITTER ***"),
				       (bDiagClean && bRowIsJitter)
				           ? TEXT("PASS: ndc = d*M00 + J, so d = (ndc - J)/M00 -- SUBTRACT is "
				                  "correct, which is what VoxelMarch.usf:869 and :1044 do")
				           : TEXT("FAIL: do not trust voxel.March.TAAJitter until this is "
				                  "explained"));
				break;
			}
		}

		// ---- PROVE IT, AND LET IT FAIL ------------------------------------
		//
		// Reports the ENGINE's phase whether the arm is on or off, because the
		// question "is there a phase to carry at all" is logically upstream of
		// the arm and must be answerable without it. The image A/B cannot answer
		// it: measured 2026-08-26, the quad path at this project's vista pose
		// draws only a smooth untextured ridge, and a sub-pixel shift on a
		// smooth gradient moves no pixel by a readable amount. A number can.
		//
		// THE INERT CASE IS REAL AND IS NOT ALWAYS A BUG: with no temporal AA
		// method active UE never jitters at all, GetTemporalAAJitter() is
		// honestly (0,0), and an A/B taken in that state compares the arm
		// against ITSELF while every switch reads as armed. That is the failure
		// this block exists to make loud -- it has already caught one.
		// A WINDOW, NOT THE FIRST FEW FRAMES, AND THAT DISTINCTION COST A BUILD.
		//
		// The first version logged the first 8 DISTINCT offsets and stopped. All
		// eight land in the first eight rendered frames -- before -ExecCmds has
		// run -- so it reported the state at BOOT and could never report the
		// state at the SHUTTER. It printed "arm is OFF" on a run launched with
		// voxel.March.TAAJitter 1, and showed a free-running sequence on a run
		// launched with r.TemporalAA.Debug.OverrideTemporalIndex 5, purely
		// because both cvars land later. Neither reading was about the
		// configuration under test.
		//
		// So the settled instrument is a WINDOW COUNT: how many times the offset
		// CHANGED over the last kJitterWindow frames. Free-running that is ~the
		// window size; pinned by OverrideTemporalIndex it is exactly 0 with a
		// non-zero offset; with no temporal AA at all it is 0 with a zero
		// offset. Three states, three signatures, none of them silent. Same
		// argument the half-res arm's 'phasesSeen on a settled leg' makes.
		constexpr int32 kJitterWindow = 300;
		static int32 DistinctLogged = 0;
		static FVector2D LastLogged(3.4e+38, 3.4e+38);
		static int32 ZeroFrames = 0;
		static bool bZeroComplained = false;
		static bool bLastArmed = false;
		static int32 WindowFrames = 0;
		static int32 WindowChanges = 0;
		static FVector2D WindowPrev(3.4e+38, 3.4e+38);
		// The SAME window over the resolved uniform. Two counters and not one,
		// because they answer two different questions and only the second is
		// about this arm: "is there a phase to carry" (engine) vs "does the
		// value the shader reads actually move" (resolved). With the arm ON
		// they must agree; with it OFF the resolved count must be 0 at a zero
		// offset while the engine count is still ~the window. If the engine
		// count is high and the resolved count is 0 while the arm reads ON,
		// the write above is not reaching the entry the binds read -- which is
		// precisely the silent-inert failure this whole block exists for.
		static int32 WindowResolvedChanges = 0;
		static FVector2f WindowResolvedPrev(3.4e+38f, 3.4e+38f);

		// Re-open the distinct log whenever the ARM ITSELF flips, so the frame
		// -ExecCmds turns this on prints with "ON" instead of being swallowed by
		// a counter that filled up during startup.
		if (bArmed != bLastArmed)
		{
			bLastArmed = bArmed;
			DistinctLogged = 0;
			LastLogged = FVector2D(3.4e+38, 3.4e+38);
		}

		++WindowFrames;
		if (FrameJitter != WindowPrev)
		{
			++WindowChanges;
			WindowPrev = FrameJitter;
		}
		if (ResolvedJitter != WindowResolvedPrev)
		{
			++WindowResolvedChanges;
			WindowResolvedPrev = ResolvedJitter;
		}
		if (WindowFrames >= kJitterWindow)
		{
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("[voxel-march] taajitter window: %d frames, engine offset CHANGED %d "
			            "times | UNIFORM (what the shader reads) CHANGED %d times, now "
			            "(%+.6f, %+.6f) | current engine (%+.6f, %+.6f) NDC | arm %s | %s | %s"),
			       WindowFrames, WindowChanges, WindowResolvedChanges, ResolvedJitter.X,
			       ResolvedJitter.Y, FrameJitter.X, FrameJitter.Y,
			       bArmed ? TEXT("ON") : TEXT("OFF"),
			       (bArmed && WindowResolvedChanges <= 1)
			           ? TEXT("*** UNIFORM IS STUCK -- arm reads ON but the value the binds copy "
			                  "did not move across the window: SILENTLY INERT, do not trust any "
			                  "image from this leg ***")
			           : (bArmed ? TEXT("UNIFORM IS LIVE -- the marched ray carries a moving "
			                            "sub-pixel phase")
			                     : TEXT("uniform held at zero, as arm 0 requires")),
			       WindowChanges == 0
			           ? (FrameJitter.IsNearlyZero()
			                  ? TEXT("STATIC AT ZERO -- no temporal AA is running, so this arm "
			                         "cannot do anything and an A/B here is arm-vs-itself")
			                  : TEXT("PINNED -- a fixed non-zero phase, i.e. "
			                         "r.TemporalAA.Debug.OverrideTemporalIndex is in force"))
			           : TEXT("FREE-RUNNING -- the engine sequence is live"));
			WindowFrames = 0;
			WindowChanges = 0;
			WindowResolvedChanges = 0;
		}

		if (FrameJitter != LastLogged && DistinctLogged < 8)
		{
			LastLogged = FrameJitter;
			++DistinctLogged;
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("[voxel-march] taajitter: engine phase #%d = (%+.6f, %+.6f) NDC = "
			            "(%+.3f, %+.3f) px of a %dx%d view | marcher arm is %s"),
			       DistinctLogged, FrameJitter.X, FrameJitter.Y,
			       FrameJitter.X * 0.5 * double(FMath::Max(1, JitterRect.Width())),
			       FrameJitter.Y * -0.5 * double(FMath::Max(1, JitterRect.Height())),
			       JitterRect.Width(), JitterRect.Height(),
			       bArmed
			           ? TEXT("ON (voxel.March.TAAJitter 1) -- this offset is subtracted from "
			                  "Ndc in BOTH VoxelMarchBuildRay and VoxelMarchSampleDirWorld")
			           : TEXT("OFF (voxel.March.TAAJitter 0) -- the ray ignores this offset "
			                  "entirely, which is the shipping behaviour"));
		}

		if (FrameJitter.IsNearlyZero())
		{
			++ZeroFrames;
			if (ZeroFrames == 120 && !bZeroComplained)
			{
				bZeroComplained = true;
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("[voxel-march] taajitter: NO ENGINE JITTER -- 120 consecutive frames "
				            "at exactly (0,0), read INSIDE the base pass where the jitter does "
				            "exist. UE only jitters the projection when a temporal AA method is "
				            "active, so check r.AntiAliasingMethod and r.TemporalAASamples. "
				            "voxel.March.TAAJitter CANNOT DO ANYTHING in this state, and an A/B "
				            "taken here compares the arm against itself."));
			}
		}
		else
		{
			ZeroFrames = 0;
		}
	}

	TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe> Volume;
	{
		FScopeLock Guard(&State->Lock);
		Volume = State->Volume;
	}
	if (!Volume.IsValid() && VoxelMarchNeedsOccupancyVolume(VoxelMarchGetArm()))
	{
		FScopeLock Guard(&State->Lock);
		State->Stats.DeclinedNoVolume++;
		return;
	}

	// REGISTRATION WITHOUT CLEAR OR FILL. The volume's own AddPasses already ran
	// this frame, inside the fluid solver's graph build at PreRenderViewFamily.
	// This is a reader in a later pass of the same graph and it must not touch
	// the bits. Null means AddPasses has never run -- the buffer does not exist
	// and binding a null SRV is an RDG assert, not a black frame.
	if (Volume.IsValid() && Volume->Register(GraphBuilder) == nullptr)
	{
		// PRESENT BUT UNBUILT -- AddPasses has never run, so there is no buffer.
		// Source 0 has nothing to march and declines. Source 1 drops it and takes
		// the stand-in binding; declining there would put terrain rendering back
		// behind the fluid solver having run, which is the whole point of this.
		if (VoxelMarchNeedsOccupancyVolume(VoxelMarchGetArm()))
		{
			FScopeLock Guard(&State->Lock);
			State->Stats.DeclinedNoVolume++;
			return;
		}
		Volume.Reset();
	}

	// HOW LONG THE VOLUME HAS BEEN QUIET. Once per frame, before any view: the
	// index is global, so this is a property of the frame and not of a view.
	{
		const uint64 Now = GetGlobalVoxelMarchChunkIndex().GetUploads();
		if (Now != State->LastSeenIndexUploads)
		{
			State->LastSeenIndexUploads = Now;
			State->FramesSinceIndexChanged = 0;
		}
		else
		{
			State->FramesSinceIndexChanged++;
		}
		FScopeLock Guard(&State->Lock);
		State->Stats.IndexQuietFrames = State->FramesSinceIndexChanged;
	}

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelMarch, "VoxelMarch");

	// ---- HALF RES: ONE READ, ABOVE THE VIEW LOOP ---------------------------
	//
	// voxel.March.HalfRes decides the size of the VisBuffer, the shape of the
	// dispatch, the size of the emit's tile quad and which permutation of five
	// shaders runs. Every one of those has to describe the SAME buffer, so the
	// cvar is read ONCE for the frame and every view in the family gets the same
	// answer. A per-view read would let a split-screen family march one view at
	// half res and rasterise it with the other view's tile size, and the symptom
	// would be three quarters of the terrain missing from one viewport.
	//
	// The EMIT hook does not read this at all -- it derives the same bit from
	// the extent of the VisBuffer the march actually wrote, which is the one
	// thing that cannot have drifted. See PostRenderBasePassDeferred.
	const int32 ResShift = (CVarVoxelMarchHalfRes.GetValueOnRenderThread() != 0) ? 1 : 0;

	// ---- THE SAMPLE LATTICE, RESOLVED ONCE FOR THE FRAME -------------------
	//
	// Above the view loop for the same reason ResShift is: every view in a
	// family gets the same lattice, and the emit and the gate re-derive nothing
	// -- they read this frame's stamp. Full res forces it to zero here as well
	// as in the .usf's #if, so neither half can arm it alone.
	const FVoxelMarchJitter Jitter =
		VoxelMarchResolveJitter(ResShift != 0, GFrameNumberRenderThread);

	// ---- PROOF OF TRAFFIC FOR THE MOVING LATTICE ---------------------------
	//
	// ONE LINE PER DISTINCT PHASE ACTUALLY DISPATCHED, then silent: four lines
	// under scheme 1, eight under scheme 2, and EXACTLY ONE if the lattice is
	// static. So a static arm and a jittered one are distinguishable FROM THE
	// RAW LOG ALONE -- no console command, no readback, no counting frames.
	// Reset when the scheme changes, so a live flip re-prints its own cycle.
	//
	// IT PRINTS THE OFFSET IN FLOATING POINT, not merely the phase index,
	// because a phase index that advances while the offset table is wrong would
	// still read as motion. The pair of numbers in this line IS the number the
	// uniform carries, taken from the same VoxelMarchJitterOffset call the
	// dispatch below is about to use.
	//
	// READ IT WITH voxel.March.Stats 'halfResJitter', which prints the running
	// phase mask: this line proves the phases were dispatched at least once,
	// that one proves they are still cycling on a settled leg.
	{
		static int32 LoggedJitterScheme = -2;
		static uint32 LoggedJitterPhaseMask = 0u;
		if (Jitter.Scheme != LoggedJitterScheme)
		{
			LoggedJitterScheme = Jitter.Scheme;
			LoggedJitterPhaseMask = 0u;
			GVoxelMarchRanJitterPhaseMask.store(0u, std::memory_order_relaxed);
			GVoxelMarchRanJitterFrames.store(0, std::memory_order_relaxed);
		}
		const uint32 PhaseBit = 1u << uint32(Jitter.Phase);
		const bool bPhasePinned =
			CVarVoxelMarchHalfResJitterPhase.GetValueOnRenderThread() >= 0;
		if ((LoggedJitterPhaseMask & PhaseBit) == 0u)
		{
			LoggedJitterPhaseMask |= PhaseBit;
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("[voxel-march] jitter: halfRes=%d scheme=%d (%s) | phase %d of %d | "
			            "sample = 2x2 block centre + (%+.4f, %+.4f) full-res px | %s"),
			       ResShift, Jitter.Scheme,
			       Jitter.Scheme == 0
			           ? TEXT("STATIC block centre")
			           : (Jitter.Scheme == 1
			                  ? TEXT("4-frame rotation over the four full-res pixel centres")
			                  : TEXT("8-frame Halton(2,3) over the same +/-0.5 px box")),
			       Jitter.Phase, Jitter.Cycle, Jitter.Offset.X, Jitter.Offset.Y,
			       bPhasePinned
			           ? TEXT("PHASE PINNED by voxel.March.HalfRes.JitterPhase -- a measurement "
			                  "mode, and a pinned lattice is a STATIC one")
			           : TEXT("free-running on the render-thread frame number"));
		}
		GVoxelMarchJitterStampFrame.store(GFrameNumberRenderThread, std::memory_order_relaxed);
		GVoxelMarchRanJitterScheme.store(Jitter.Scheme, std::memory_order_relaxed);
		GVoxelMarchRanJitterPhase.store(Jitter.Phase, std::memory_order_relaxed);
		GVoxelMarchRanJitterCycle.store(Jitter.Cycle, std::memory_order_relaxed);
		GVoxelMarchRanJitterPhaseMask.fetch_or(PhaseBit, std::memory_order_relaxed);
		GVoxelMarchRanJitterFrames.fetch_add(1, std::memory_order_relaxed);
	}

	FVoxelMarchState::FTimingPair* Timing = OpenBracket(GraphBuilder, State->MarchTiming, TEXT("March"));

	uint32 TotalTiles = 0;

	for (FViewMarch& Entry : Views)
	{
		if (Entry.FrameNumber != GFrameNumberRenderThread)
		{
			continue;
		}

		// Scene textures, via the public accessor. SceneDepth here is the
		// PREPASS depth and it is what bounds every ray: a ray that would run to
		// the far plane stops at whatever the prepass already drew in that pixel.
		// That is not an optimisation bolted on -- it is the reason the march can
		// afford to run before the base pass at all.
		const TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUB =
			UE::FXRenderingUtils::GetOrCreateSceneTextureUniformBuffer(
				GraphBuilder, MakeStridedView(int32(sizeof(FSceneView)), Entry.ViewKey, 1),
				GMaxRHIFeatureLevel, ESceneTextureSetupMode::SceneDepth);
		if (SceneTexturesUB == nullptr)
		{
			FScopeLock Guard(&State->Lock);
			State->Stats.DeclinedNoTextures++;
			continue;
		}
		FRDGTextureRef SceneDepth = SceneTexturesUB->GetContents()->SceneDepthTexture;
		if (SceneDepth == nullptr)
		{
			FScopeLock Guard(&State->Lock);
			State->Stats.DeclinedNoTextures++;
			continue;
		}

		const FIntPoint Size = Entry.ViewRect.Size();

		// ---- THREE SIZES, AND EACH ONE ANSWERS A DIFFERENT QUESTION -------
		//
		//   Size          the view rect, in screen pixels. What the emit
		//                 rasterises and what the depth gate walks.
		//   MarchSize     the march's SAMPLE GRID. Equal to Size at full res;
		//                 half of it, ROUNDED UP, at half res. This is the
		//                 VisBuffer's extent and the ray count.
		//   TileCount     the march's GROUP grid, which is also the emit's tile
		//                 grid. The group is 8x8 THREADS at both resolutions --
		//                 the group IS the tile-hit reduction and that is not
		//                 negotiable -- so at half res one group owns a 16x16
		//                 FULL-RES tile and the emit's quad grows to match
		//                 (VOXEL_MARCH_FULL_TILE_SIZE in the shader).
		//
		// The rounding agrees by identity rather than by luck:
		// ceil(ceil(W/2)/8) == ceil(W/16). So the tile grid derived from the
		// sample grid is the same grid the emit would get from dividing the
		// screen by 16, and the two halves cannot disagree about how many tiles
		// there are -- which they must not, because MarchOutTileHit is indexed
		// by group id and read by the compaction as a flat array.
		const int32 ResDiv = 1 << ResShift;
		const FIntPoint MarchSize(FMath::DivideAndRoundUp(Size.X, ResDiv),
		                          FMath::DivideAndRoundUp(Size.Y, ResDiv));
		const FIntPoint TileCount(FMath::DivideAndRoundUp(MarchSize.X, kVoxelMarchTileSize),
		                          FMath::DivideAndRoundUp(MarchSize.Y, kVoxelMarchTileSize));
		// The FULL-RES tile grid, for the diagnostics that still run one thread
		// per SCREEN pixel. The source comparator is the one that matters: it
		// marches every screen pixel three times and compares populations, so
		// dispatching it over the half-res grid would silently halve the
		// population it judges and every rate it prints would describe a quarter
		// of the frame while looking like a whole one.
		const FIntPoint FullResTileCount(FMath::DivideAndRoundUp(Size.X, kVoxelMarchTileSize),
		                                 FMath::DivideAndRoundUp(Size.Y, kVoxelMarchTileSize));
		Entry.TileCount = TileCount;
		const uint32 TileTotal = uint32(TileCount.X) * uint32(TileCount.Y);
		TotalTiles += TileTotal;

		// THE VISIBILITY BUFFER, and why it is two textures and not one.
		//
		//   Vis   uint2  -- the voxel, the face, the level, the material and the
		//                   eight AO bits. Packed; VoxelMarch.usf owns the layout
		//                   and is the only file that touches it.
		//   HitT  float  -- THE RAY'S FLOAT t, IN UU, UNQUANTISED.
		//
		// The float t is stored rather than re-derived, and it is the single most
		// load-bearing choice in this pass. Depth AND velocity are both
		// reconstructed from it. Reconstructing the hit from a 24-bit reverse-Z
		// depth instead would quantise the previous-frame position more coarsely
		// than the motion vectors it feeds, and TSR would then smear a static
		// world -- the exact failure the plan's item 2 warns about, arriving by a
		// different route.
		//
		// Derived from the VIEW RECT and indexed in view-local coordinates, not
		// from the scene extent. A split-screen or stereo family gets one of
		// these per view and they cannot alias.
		//
		// AND SIZED TO THE MARCH SAMPLE GRID RATHER THAN TO THE RECT. At full res
		// those are the same number. At half res this buffer is physically a
		// quarter of the pixels, which is what makes voxel.March.HalfRes
		// impossible to arm without engaging: a shader compiled for the other
		// side of the switch does not render slightly differently, it indexes a
		// texture that is not the size it thinks it is.
		FRDGTextureRef Vis = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(MarchSize, PF_R32G32_UINT, FClearValueBinding::None,
			                          TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("VoxelMarch.Vis"));
		FRDGTextureRef HitT = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(MarchSize, PF_R32_FLOAT, FClearValueBinding::None,
			                          TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("VoxelMarch.HitT"));

		// ---- PROOF OF TRAFFIC ---------------------------------------------
		//
		// UNCONDITIONAL, and it prints what the pass IS ABOUT TO RUN rather than
		// what any cvar says. This project has had nine switches turn out
		// inert-but-armed in two days, including one that ran the same
		// configuration twice and read as a clean null result; the discipline
		// that caught that was the raster atlas's "fill: mode=N" line, and this
		// is that line for this arm.
		//
		// Printed on CHANGE rather than every frame -- a per-frame line at 100+
		// fps buries the log -- but the change set includes the view size, so a
		// resolution change or a screen-percentage sweep re-prints it and a leg
		// always has one line per configuration it actually ran.
		//
		// READ IT WITH TWO OTHER THINGS, because a CPU log line only proves what
		// was DISPATCHED: voxel.March.Stats 'tiles drawn' is a GPU READBACK of
		// the compaction's own output and falls ~4x on this arm, and
		// voxel.March.VerifyDepth in mode 2 grades the reconstruction itself.
		{
			static int32 LoggedSampleW = -1;
			static int32 LoggedSampleH = -1;
			static int32 LoggedViewW = -1;
			static int32 LoggedViewH = -1;
			static int32 LoggedShift = -2;
			// THE LATTICE IS PART OF THE SHAPE THIS LINE DESCRIBES, so a flip of
			// voxel.March.HalfRes.Jitter re-prints it and no leg can read a
			// half-res line and assume the static lattice it used to mean. The
			// PHASE is deliberately NOT in the change set -- it moves every
			// frame and would bury the log; the '[voxel-march] jitter:' line
			// above carries the phase, one line per distinct phase dispatched.
			static int32 LoggedJitterSchemeOnLine = -2;
			if (MarchSize.X != LoggedSampleW || MarchSize.Y != LoggedSampleH ||
			    Size.X != LoggedViewW || Size.Y != LoggedViewH || ResShift != LoggedShift ||
			    Jitter.Scheme != LoggedJitterSchemeOnLine)
			{
				LoggedJitterSchemeOnLine = Jitter.Scheme;
				LoggedSampleW = MarchSize.X;
				LoggedSampleH = MarchSize.Y;
				LoggedViewW = Size.X;
				LoggedViewH = Size.Y;
				LoggedShift = ResShift;
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("[voxel-march] halfres: arm=%d | view=%dx%d px | rays=%dx%d "
				            "(%.3f Mray, %.2fx the pixels) | dispatch=%dx%d groups of %dx%d "
				            "threads | tile=%d full-res px, tiles=%dx%d=%u | Vis=%dx%d "
				            "HitT=%dx%d | lattice=%s | emit+depth+gate read the VisBuffer by: %s"),
				       ResShift != 0 ? 1 : 0, Size.X, Size.Y, MarchSize.X, MarchSize.Y,
				       double(MarchSize.X) * double(MarchSize.Y) / 1.0e6,
				       double(MarchSize.X) * double(MarchSize.Y) /
				           FMath::Max(1.0, double(Size.X) * double(Size.Y)),
				       TileCount.X, TileCount.Y, kVoxelMarchTileSize, kVoxelMarchTileSize,
				       kVoxelMarchTileSize << ResShift, TileCount.X, TileCount.Y, TileTotal,
				       Vis->Desc.Extent.X, Vis->Desc.Extent.Y, HitT->Desc.Extent.X,
				       HitT->Desc.Extent.Y,
				       ResShift == 0
				           ? TEXT("full res -- one ray per pixel centre")
				           : (Jitter.Scheme == 0
				                  ? TEXT("STATIC -- sample at the 2x2 block centre every "
				                         "frame (voxel.March.HalfRes.Jitter 0)")
				                  : (Jitter.Scheme == 1
				                         ? TEXT("MOVING -- 4-frame rotation over the four "
				                                "full-res pixel centres")
				                         : TEXT("MOVING -- 8-frame Halton(2,3) inside the "
				                                "2x2 block"))),
				       ResShift != 0
				           ? TEXT("EXACT RAY/PLANE RECONSTRUCTION over the 2x2 sample "
				                  "neighbourhood, nearest-neighbour fallback where no candidate "
				                  "face covers the pixel")
				           : TEXT("a direct Load at the pixel's own texel (no reconstruction)"));
			}
			GVoxelMarchRanSampleWidth.store(MarchSize.X, std::memory_order_relaxed);
			GVoxelMarchRanSampleHeight.store(MarchSize.Y, std::memory_order_relaxed);
			GVoxelMarchRanViewWidth.store(Size.X, std::memory_order_relaxed);
			GVoxelMarchRanViewHeight.store(Size.Y, std::memory_order_relaxed);
			GVoxelMarchRanResShift.store(ResShift, std::memory_order_relaxed);
		}

		// ---- the reach, and the frame it implies ------------------------
		//
		// Reach 0 keeps the occupancy volume's own frame and box: origin at its
		// min corner, extent 51.2 m. That is the ONLY configuration in which the
		// source A/B is valid, because the occupancy volume cannot follow a
		// longer ray.
		//
		// A longer reach re-anchors the frame on the CAMERA -- origin at
		// (camera - reach), snapped DOWN to a chunk boundary so chunk-coordinate
		// arithmetic stays exact -- and sets the box to 2 x reach. R0 spans
		// 0-128 m, so level 0 alone supports this with no ring-transition logic.
		//
		// Snapping matters: the frame origin is what world voxels are derived
		// from, and an unaligned origin would put the marcher's chunk boundaries
		// half a chunk away from the pool's.
		// ---- THE FRAME, AND WHY SOURCE 1 NO LONGER BORROWS THE FLUID'S ----
		//
		// Source 0 must keep the volume's frame: VoxelFluidWalkVoxelLine resolves
		// world voxels as `localStart + FluidVolumeOriginVoxel`
		// (VoxelFluidCollision.ush:325), so its local frame IS the volume frame
		// by construction. That file is the fluid contract and is not ours to
		// change.
		//
		// Source 1 has no such tie -- the brick path takes world voxels and the
		// chunk index is global -- and borrowing the fluid's frame was costing us
		// the ability to measure anything. The volume's origin runs through
		// RecentreTo's hysteresis, so it depends on the history of camera motion
		// and on whether a recentre happened to land this run; two identical legs
		// got different clip boxes and therefore different rays.
		//
		// Camera-anchored and chunk-snapped, the frame is a pure function of the
		// pose. The snap matters twice: it keeps chunk arithmetic exact, and it
		// makes the origin insensitive to sub-chunk camera jitter.
		// SEEDED FROM THE VOLUME ONLY WHEN THERE IS ONE. Source 1 overwrites this
		// below; with no volume the old default dereferenced null.
		FIntVector FrameOriginVoxel =
			Volume.IsValid() ? Volume->GetOriginVoxel() : FIntVector::ZeroValue;
		FVector3f FrameRayOriginLocalUU = Entry.RayOriginLocalUU;
		float FrameExtentUU = float(FVoxelFluidOccupancyVolume::DimVoxels) * vxc::kFluidVoxelUU;
		// True only while the march frame IS the volume frame -- i.e. source 0.
		// The comparator's occupancy half is meaningless otherwise and is gated
		// on this rather than left to produce confident nonsense.
		// FALSE WITHOUT A VOLUME, not merely under source 1. This gates the
		// comparator's occupancy half; left true on a no-volume path it would
		// claim that half valid over a stand-in buffer.
		bool bFrameIsVolumeFrame = Volume.IsValid();
		if (Arm.Source == 1)
		{
			// Default 25.6 m preserves the geometry every number so far was taken
			// at (the 512-voxel box, camera near its centre), so the swap changes
			// determinism without changing what is being measured.
			// With the cascade on, the reach is the OUTER RADIUS OF THE LAST
			// RING and nothing else -- one authority for how far the marcher
			// sees, shared with the shader's segments. A ReachM that disagreed
			// with the rings would clip the outer ring silently.
			const FVoxelMarchRingSpec Rings = VoxelMarchGetRingSpec();
			const float ReachM = Rings.bEnabled
				? (Rings.ReachUU / 100.0f)
				: ((Arm.ReachM > 0.0f) ? Arm.ReachM : 25.6f);
			const int32 ReachVoxels = FMath::CeilToInt(ReachM * 10.0f);
			// SNAPPED TO 1024 LEVEL-0 VOXELS WHEN RINGS ARE ON, 32 OTHERWISE.
			// A chunk at level L spans 32 * 2^L level-0 voxels; 1024 is that at
			// L5, so a 1024-snapped origin is chunk-aligned AT EVERY LEVEL and
			// `origin >> L` stays exact. Without it every level above 0 needs a
			// per-level fixup, and a frame origin that is chunk-aligned "only by
			// luck" is exactly the class of thing this file has been bitten by.
			// Costs at most 102 m of frame offset, which the box already covers.
			// Kept at 32 with rings off so the control arm is unchanged.
			const int32 SnapShift = Rings.bEnabled ? 10 : 5;
			const auto SnapDown = [SnapShift](int32 V) { return (V >> SnapShift) << SnapShift; };
			// UNDER RINGS THE FRAME ORIGIN IS THE CAMERA, NOT THE BOX CORNER,
			// and the shader's box is centred on it to match. Cornered, the
			// camera sits one full reach from local zero -- 409,600 UU at 4 km,
			// where a float32 ulp is 0.049 UU against a 0.01 UU advance nudge.
			// The nudge rounds away and the walk stalls or skips, silently.
			// Centred, |origin| is at most the snap slack and every ring's
			// positions stay within its own outer radius.
			const FIntVector NewOrigin =
				Rings.bEnabled
					? FIntVector(SnapDown(Entry.CameraVoxel.X), SnapDown(Entry.CameraVoxel.Y),
					             SnapDown(Entry.CameraVoxel.Z))
					: FIntVector(SnapDown(Entry.CameraVoxel.X - ReachVoxels),
					             SnapDown(Entry.CameraVoxel.Y - ReachVoxels),
					             SnapDown(Entry.CameraVoxel.Z - ReachVoxels));
			// Differenced in DOUBLE against the true camera position, not shifted
			// from the old frame -- the old frame is exactly the quantity being
			// removed from the measurement.
			const FVector OriginUU(double(NewOrigin.X) * 10.0, double(NewOrigin.Y) * 10.0,
			                       double(NewOrigin.Z) * 10.0);
			FrameRayOriginLocalUU = FVector3f(Entry.ViewOriginUU - OriginUU);
			FrameOriginVoxel = NewOrigin;
			// Twice the snap, so the box covers the worst-case snap slack in
			// both directions: the origin can sit up to (1 << SnapShift) - 1
			// voxels further back than reach alone would need. 64 was that at a
			// 32-snap; the 1024-snap needs 2048 or the outer ring is clipped by
			// the box before the rings ever get to decide.
			// FULL WIDTH either way. Cornered, the box runs [0, extent]; centred,
			// it runs [-extent/2, +extent/2] -- the same span, and the shader
			// picks which under the same permutation that moved the origin.
			FrameExtentUU = float(2 * ReachVoxels + 2 * (1 << SnapShift)) * 10.0f;
			// THE PACK BOUND IS PER LEVEL NOW, AND CHECKED RATHER THAN TRUSTED.
			//
			// The old check compared the whole frame span against 8,192 voxels.
			// That was right while the pack was frame-relative; with six rings
			// the frame spans ~84,000 level-0 voxels and the check would fail
			// permanently while nothing was actually wrong -- the pack became
			// CAMERA-relative at the hit's own level precisely so it does not
			// scale with reach.
			//
			// What must hold now is per ring: a ring-L hit is at most
			// outer(L) / voxelSize(L) voxels from the camera, and because the
			// cascade doubles both together that is the SAME 1,280 at every
			// level. Against a biased 13-bit field (-4096..+4095) the margin is
			// 3.2x. Verified per ring anyway, because the thing that makes this
			// safe is an invariant of the ring presets and those are
			// command-line overridable.
			// REACHABILITY, CHECKED BEFORE THE LEG RATHER THAN AFTER A ZERO.
			// Printed once per (budget, ringCount) so it lands in the log the
			// leg is read from, not in a separate place someone has to think to
			// look at.
			if (Rings.bEnabled)
			{
				static int32 LastBudget = -1;
				static int32 LastCount = -1;
				const int32 Budget = Arm.StepBudget;
				if (Budget != LastBudget || Rings.Count != LastCount)
				{
					LastBudget = Budget;
					LastCount = Rings.Count;
					const FVoxelMarchRingReach Reach = VoxelMarchCheckRingReach(Rings, Budget);
					UE_LOG(LogVoxelMarch, Display,
					       TEXT("Voxel march ring reach: %d rings, budget %d per traversal "
					            "call (NOT per ray -- each ring is a separate call and gets "
					            "its own allowance). Worst single ring needs %d steps."),
					       Rings.Count, Budget, Reach.WorstStepsForRing);
					if (Reach.FirstUnreachable >= 0)
					{
						UE_LOG(LogVoxelMarch, Error,
						       TEXT("Voxel march ring reach: RING %d CANNOT BE CROSSED AT "
						            "BUDGET %d. It and every ring beyond it will report ZERO "
						            "for a reason no code change can move, and that zero will "
						            "look like a defect. Raise the budget to at least %d or "
						            "lower voxel.March.RingCount -- do NOT read a ring that "
						            "arithmetic says is out of range."),
						       Reach.FirstUnreachable, Budget, Reach.WorstStepsForRing);
					}
				}
				static bool bPackWarned = false;
				for (int32 L = 0; L < Rings.Count && !bPackWarned; ++L)
				{
					// Level-L voxel size in UU, and the ring's reach in those voxels.
					const double VoxelUU = 10.0 * double(1 << L);
					const int32 ReachInLevelVoxels =
						FMath::CeilToInt(double(Rings.OuterUU(L)) / VoxelUU);
					if (ReachInLevelVoxels > 4095)
					{
						bPackWarned = true;
						UE_LOG(LogVoxelMarch, Error,
						       TEXT("Voxel march ring %d reaches %d level-%d voxels from the "
						            "camera, past the vis pack's +/-4095. Hits beyond it WRAP "
						            "to a legal-looking coordinate and draw terrain in the "
						            "wrong place with no other symptom. The cascade normally "
						            "keeps this at 1280 for every ring; a "
						            "-VoxelRing*Meters= override breaks that invariant."),
						       L, ReachInLevelVoxels, L);
					}
				}
			}
			bFrameIsVolumeFrame = false;
		}

		Entry.FrameOriginVoxel = FrameOriginVoxel;
		Entry.FrameRayOriginLocalUU = FrameRayOriginLocalUU;
		Entry.FrameExtentUU = FrameExtentUU;

		// THE VIEW BLOCK, BUILT ONCE AND SHARED. The march and the source
		// comparator must trace THE SAME RAY, and a second construction of "the
		// same" ray is exactly how two instruments end up describing two
		// populations -- the failure VoxelMarchSpike.usf's census was designed
		// around ("the same rays, not merely similar ones").
		FVoxelMarchViewParameters MarchView;
		MarchView.MarchViewToTranslatedWorld = Entry.ViewToTranslatedWorld;
		MarchView.MarchRayOriginLocalUU = FrameRayOriginLocalUU;
		MarchView.MarchVolumeExtentUU = FrameExtentUU;
		MarchView.MarchViewRectMin = FVector2f(Entry.ViewRect.Min);
		MarchView.MarchViewRectSize = FVector2f(Size);
		MarchView.MarchInvProjDiag = Entry.InvProjDiag;
		MarchView.MarchTemporalAAJitter = Entry.TemporalAAJitter;
		MarchView.MarchInvDeviceZToWorldZ = Entry.InvDeviceZToWorldZ;
		MarchView.MarchPixelConeSlope = Entry.PixelConeSlope;
		MarchView.MarchClimateStrength = CVarVoxelMarchClimateStrength.GetValueOnRenderThread();
		// The frame's one lattice, resolved above the loop. The depth pre-emit
		// and the source comparator copy this whole struct, so they cannot get a
		// different sample point than the march did.
		MarchView.MarchSampleJitter = Jitter.Offset;
		MarchView.MarchTileCount = TileCount;

		// ---- THE PER-RAY RESIDENT-EXTENT BOUND'S PRODUCER (voxel.March.Bound)
		//
		// Added IMMEDIATELY BEFORE THE MARCH PASS, only when the arm is on,
		// and OUTSIDE the March timing bracket, in its own (VoxelMarch.
		// TimeBegin/End(Bound)) -- an arm whose prepass hides inside marchMs
		// cannot be judged, and marchMs must keep meaning what every archived
		// leg measured. The producer reads the SAME chunk table the marcher
		// validates hits against and the SAME MarchView block this dispatch
		// is about to bind, both copied from the single authorities above so
		// the bound and the march cannot describe two different frames.
		//
		// BoundTex NON-NULL IS THE ONE SIGNAL the permutation below keys on:
		// cvar armed but producer declined (pool never flushed, rings off
		// this frame, half res on) means control kernel, not a bound kernel
		// reading an unbound texture -- whose zeros decode as EMPTY intervals
		// and would skip the world.
		FRDGTextureRef BoundTex = nullptr;
		uint32 BoundSliceCount = 0;
		if (Arm.Bound != 0 && Arm.bRings)
		{
			if (ResShift != 0)
			{
				// The third refused pairing, enforced here because ResShift
				// is resolved per frame on the render thread. Logged once:
				// a silent suppression would read as a bound null result.
				static bool bBoundHalfResWarned = false;
				if (!bBoundHalfResWarned)
				{
					bBoundHalfResWarned = true;
					UE_LOG(LogVoxelMarch, Warning,
					       TEXT("voxel.March.Bound is FORCED OFF while voxel.March.HalfRes "
					            "is on: the half-res sample lattice is not the raster "
					            "pixel-centre lattice, so the produced bound would describe "
					            "a different ray than the one marched. This frame (and every "
					            "frame until half res is off) runs the CONTROL -- do not "
					            "read it as a bound result."));
				}
			}
			else
			{
				// The ring count, resolved the way VoxelMarchBindPool
				// resolves the MarchRingCount uniform (the same
				// VoxelMarchGetRingSpec, same thread, same frame), so the
				// consumer's loop and the producer's slice count cannot
				// disagree about how many levels exist.
				const FVoxelMarchRingSpec BoundRings = VoxelMarchGetRingSpec();
				if (BoundRings.bEnabled && BoundRings.Count > 0)
				{
					FVoxelMarchState::FTimingPair* BoundTiming =
						OpenBracket(GraphBuilder, State->BoundTiming, TEXT("Bound"));
					FVoxelMarchBoundInputs BoundIn;
					// COPIED FROM MarchView, NOT REDERIVED. These seven are
					// the exact uniforms VoxelMarchBuildRay reads, and the
					// producer's PS runs that function verbatim; a second
					// derivation of any of them is how the bound comes to
					// describe a ray the march never traced.
					BoundIn.ViewToTranslatedWorld = MarchView.MarchViewToTranslatedWorld;
					BoundIn.RayOriginLocalUU = MarchView.MarchRayOriginLocalUU;
					BoundIn.VolumeExtentUU = MarchView.MarchVolumeExtentUU;
					BoundIn.ViewRectMin = MarchView.MarchViewRectMin;
					BoundIn.ViewRectSize = MarchView.MarchViewRectSize;
					BoundIn.InvProjDiag = MarchView.MarchInvProjDiag;
					BoundIn.TemporalAAJitter = MarchView.MarchTemporalAAJitter;
					// The eighth copy, for the half-res dilation: the pixel
					// half-width cone slope, from the same MarchView block.
					// The producer declines on a non-positive value rather
					// than rendering an undilated half-res bound.
					BoundIn.PixelConeSlope = MarchView.MarchPixelConeSlope;
					// The march frame origin -- the value the dispatch below
					// assigns to MarchBrickOriginVoxel, from the same local.
					BoundIn.FrameOriginVoxel = FrameOriginVoxel;
					BoundIn.SliceCount = BoundRings.Count;
					// The march sample grid (== the view rect; half-res
					// MARCHING is a refused pairing, enforced above). The
					// producer renders its target at HALF of this per axis
					// since 2026-08-28 and the loader reads texel
					// (pixel >> 1) -- the halving lives entirely inside the
					// producer/consumer pair, so this stays the one
					// full-res authority.
					BoundIn.TargetSize = MarchSize;
					BoundTex = VoxelMarchBoundProduce(GraphBuilder, ShaderMap, BoundIn);
					CloseBracket(GraphBuilder, BoundTiming, TEXT("Bound"));
					if (BoundTex != nullptr)
					{
						BoundSliceCount = uint32(BoundRings.Count);
						FScopeLock Guard(&State->Lock);
						State->Stats.BoundFrames++;
					}
				}
			}
		}

		FRDGBufferRef TileHit = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(TileTotal, 1u)),
			TEXT("VoxelMarch.TileHit"));
		FRDGBufferUAVRef TileHitUAV = GraphBuilder.CreateUAV(TileHit, PF_R32_UINT);
		// Cleared every frame: the mask describes ONE frame's hits. Accumulating
		// would leave the emit rasterising tiles the camera left behind, which
		// costs HTILE decompression for nothing and hides the very saving the
		// indirect emit exists to produce.
		AddClearUAVPass(GraphBuilder, TileHitUAV, 0u);

		{
			auto* Params = GraphBuilder.AllocParameters<FVoxelMarchCSParameters>();
			// Same stand-in as the emit. FComputeShaderUtils::AddPass would clear
			// an unused member here, but the two passes share this struct and
			// must not disagree about what is bound.
			if (Volume.IsValid())
			{
				Volume->BindShaderParameters(GraphBuilder, *Params);
			}
			else
			{
				FVoxelFluidOccupancyVolume::BindNullShaderParameters(GraphBuilder, *Params);
			}
			Params->MarchView = MarchView;
			Params->MarchStepBudget = Arm.StepBudget;
			// THE BRICK POOL WORKS IN THE SAME LOCAL FRAME AS THE OCCUPANCY
			// VOLUME, and that is a measurement choice, not an accident.
			//
			// The occupancy volume's origin voxel is already in level-0 world
			// voxel coordinates at 10 cm -- exactly the lattice the pool uses at
			// level 0. Sharing it means both sources build the IDENTICAL ray
			// over the IDENTICAL segment and clip to the IDENTICAL box, so the
			// only thing that differs between the two arms is the volume being
			// asked. That is what makes marchMs(source 1) - marchMs(source 0)
			// the per-step indirection cost and not a mixture of that and a
			// different traversal length.
			//
			// The cost is that source 1 also only reaches ~25.6 m in this step,
			// well inside R0's 128 m. Extending the box is P3-B2's business,
			// arriving with the ring transitions that make the extra reach mean
			// something -- and it keeps the depth gate's population directly
			// comparable to every run recorded so far, which extending it now
			// would break.
			Params->MarchBrickOriginVoxel = FrameOriginVoxel;
			Params->MarchPackOriginVoxel = Entry.CameraVoxel;
			// THE TERRAIN HEIGHT PYRAMID. Filled here rather than in
			// VoxelMarchBindPool, and the placement is the safety argument:
			// BindPool is a template over four parameter structs and the other
			// three would then each need a Buffer<float> entry, where an
			// unbound typed buffer reads as zeros and zero at this datum is sea
			// level. Only the march kernel reads the field, so only the march
			// kernel binds it. Always called, on every arm -- the function
			// itself decides whether to enable, and it never leaves the buffer
			// unbound.
			// FROM THE FIELD JUST WRITTEN, NOT FROM FrameOriginVoxel AGAIN.
			// Assigning from the parameter itself means the height walk and the
			// brick walk provably share one origin: there is no second read of
			// the source to drift, and a change to the line above carries here
			// for free. The height walk needs its own uniform because
			// MarchBrickOriginVoxel's shader-side declaration is conditional on
			// the brick pool permutation.
			Params->MarchHeightOriginVoxel = Params->MarchBrickOriginVoxel;
			VoxelMarchFillHeightPyramid(GraphBuilder, *Params);
			int32 IndexEntries = 0;
			if (!VoxelMarchBindPool(GraphBuilder, Arm.Source, *Params, IndexEntries))
			{
				FScopeLock Guard(&State->Lock);
				State->Stats.DeclinedNoPool++;
				State->Stats.IndexEntries = IndexEntries;
				continue;
			}
			{
				FScopeLock Guard(&State->Lock);
				State->Stats.IndexEntries = IndexEntries;
			}
			// bDepthBufferIsPopulated is ShouldRenderPrePass() -- with Nanite on
			// it is always true here, but a project that turns Nanite off gets a
			// partial or absent prepass and a t_max read from it would be
			// garbage. Passed through rather than assumed.
			Params->MarchHasPrepassDepth = bDepthBufferIsPopulated ? 1 : 0;
			Params->MarchSceneDepthTexture = SceneDepth;
			Params->MarchOutVis = GraphBuilder.CreateUAV(Vis);
			Params->MarchOutHitT = GraphBuilder.CreateUAV(HitT);
			Params->MarchOutTileHit = TileHitUAV;

			// The hole metric's stats buffer. Created, cleared and bound ONLY
			// when the arm is on -- off must be free, and "free" here means no
			// buffer exists, not a buffer nobody reads. The off permutation has
			// no MarchOutHoleStats global, so the null entry is never touched.
			FRDGBufferRef HoleStats = nullptr;
			if (Arm.bHoleStats)
			{
				HoleStats = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32),
					                                 // End, not Count: the ray-bound
					                                 // census's words are appended
					                                 // after the enum (Stage 0a).
					                                 int32(VoxelMarchRayBoundWord::End)),
					TEXT("VoxelMarch.HoleStats"));
				FRDGBufferUAVRef HoleStatsUAV =
					GraphBuilder.CreateUAV(HoleStats, PF_R32_UINT);
				AddClearUAVPass(GraphBuilder, HoleStatsUAV, 0u);
				Params->MarchOutHoleStats = HoleStatsUAV;
			}

			// The per-ray resident-extent bound: texture and slice count,
			// bound only when the producer actually ran (see the BoundTex
			// note above -- the permutation keys on the same local).
			if (BoundTex != nullptr)
			{
				Params->MarchBoundTex = BoundTex;
				Params->MarchBoundSliceCount = BoundSliceCount;
			}

			FVoxelMarchCS::FPermutationDomain Permutation;
			Permutation.Set<FVoxelMarchSourceDim>(Arm.Source);
			Permutation.Set<FVoxelMarchSkipDim>(Arm.Source == 1 ? Arm.SkipLevels : 0);
			Permutation.Set<FVoxelMarchRingsDim>(Arm.bRings);
			// Already 0 when rings are off (VoxelMarchGetArm), matching the
			// refused permutation.
			Permutation.Set<FVoxelMarchFallthroughDim>(Arm.Fallthrough);
			// Already false without rings (VoxelMarchGetArm), same rule.
			Permutation.Set<FVoxelMarchHoleStatsDim>(Arm.HoleStatsLevel);
			// The coarse occupancy level. Already false without rings
			// (VoxelMarchGetArm), matching the refused permutation.
			Permutation.Set<FVoxelMarchBlockSkipDim>(Arm.bBlockSkip);
			// The retry ladder's gate. Already false without rings or without a
			// fallthrough depth (VoxelMarchGetArm), matching the refused
			// permutation.
			Permutation.Set<FVoxelMarchSkyLadderDim>(Arm.bSkyLadder);
			// The buffer this dispatch is about to write is MarchSize, so the
			// kernel that writes it must be the one compiled for MarchSize. The
			// two are set from the same ResShift, four lines apart, deliberately.
			Permutation.Set<FVoxelMarchHalfResDim>(ResShift != 0);
			// FROM THE PRODUCED TEXTURE, NOT FROM THE CVAR OR EVEN THE ARM:
			// a bound kernel may only be selected when the texture it reads
			// exists in this graph. Arm on + producer declined = control
			// kernel, logged by the producer block, never a kernel reading
			// an unbound Texture2DArray whose zeros decode as EMPTY
			// intervals (skip the world -- every ray a hole, no error).
			Permutation.Set<FVoxelMarchBoundDim>(BoundTex != nullptr);
			TShaderMapRef<FVoxelMarchCS> Shader(ShaderMap, Permutation);
			// ERDGPassFlags::NeverCull, AND IT IS NOT DEFENSIVE -- WITHOUT IT
			// MODE 2 MEASURES NOTHING.
			//
			// RDG culls any pass whose outputs nothing reads. In mode 2 the emit
			// writes a SCRATCH depth and scratch GBuffers that no later pass
			// consumes, so RDG drops the emit; the march's VisBuffer then has no
			// consumer either, so RDG drops the march too. The timing brackets
			// are already NeverCull, so both would still REPORT -- as
			// near-zero milliseconds, for a frame in which the marcher did not
			// run at all.
			//
			// That is the exact failure this project keeps naming: an instrument
			// printing a small, plausible number for work that never happened.
			// The gate mode would have looked like a spectacular result.
			//
			// (With voxel.March.VerifyDepth 1 the march survives anyway, because
			// the verify pass reads its output. Relying on that would make the
			// timing arm depend on the gate arm being on, which is precisely the
			// coupling the two cvars exist to avoid.)
			// BOTH SIZES IN THE EVENT NAME. ProfileGPU and RenderDoc show this
			// string and nothing else about the pass, so "rays for px" is where
			// a capture proves half res engaged without a log at all.
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("VoxelMarch.March(%dx%d rays for %dx%d px, budget %d)",
				               MarchSize.X, MarchSize.Y, Size.X, Size.Y, Arm.StepBudget),
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
				Shader, Params, FIntVector(TileCount.X, TileCount.Y, 1));

			// The hole metric's readback, straight after the pass that wrote
			// it -- the VoxelShadowMarch.cpp slot-ring pattern, copied rather
			// than re-invented. A frame with no free slot goes unmeasured and
			// biases nothing: the perf line divides by frames LANDED.
			if (HoleStats != nullptr)
			{
				FVoxelMarchState::FHoleReadback* Free = nullptr;
				for (FVoxelMarchState::FHoleReadback& Slot : State->HoleRing)
				{
					if (!Slot.bInFlight) { Free = &Slot; break; }
				}
				if (Free != nullptr)
				{
					if (!Free->Readback.IsValid())
					{
						Free->Readback = MakeUnique<FRHIGPUBufferReadback>(
							TEXT("VoxelMarch.HoleStatsReadback"));
					}
					AddEnqueueCopyPass(GraphBuilder, Free->Readback.Get(), HoleStats,
					                   uint32(VoxelMarchRayBoundWord::End) * sizeof(uint32));
					Free->bInFlight = true;
					// Which permutation filled this slot -- read at landing so
					// a level-1 frame's structurally zero breakdown words are
					// never folded in as measurements (see FHoleReadback).
					Free->ArmLevel = Arm.HoleStatsLevel;
				}
			}
		}

		// The emit's indirect args and its tile list. Compacted from the hit mask
		// the march just wrote, so the raster pass covers tiles that ACTUALLY HIT
		// -- not tiles that might have. Sky, and everything the prepass already
		// occluded, is never rasterised, which is the whole mitigation for
		// SV_Depth killing early-Z and invalidating HTILE.
		FRDGBufferRef TileList = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(TileTotal, 1u)),
			TEXT("VoxelMarch.TileList"));
		FRDGBufferRef DrawArgs = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateIndirectDesc<FRHIDrawIndirectParameters>(1),
			TEXT("VoxelMarch.DrawArgs"));
		FRDGBufferUAVRef DrawArgsUAV = GraphBuilder.CreateUAV(DrawArgs, PF_R32_UINT);
		AddClearUAVPass(GraphBuilder, DrawArgsUAV, 0u);

		{
			auto* Params = GraphBuilder.AllocParameters<FVoxelMarchCompactParameters>();
			Params->MarchTileCount = TileCount;
			Params->MarchTileTotal = TileTotal;
			Params->MarchTileHit = GraphBuilder.CreateSRV(TileHit, PF_R32_UINT);
			Params->MarchOutTileList = GraphBuilder.CreateUAV(TileList, PF_R32_UINT);
			Params->MarchOutDrawArgs = DrawArgsUAV;

			TShaderMapRef<FVoxelMarchCompactCS> Shader(ShaderMap);
			// NeverCull for the same reason as the march above: in mode 2 its
			// only consumer is a draw that RDG would otherwise remove.
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("VoxelMarch.CompactTiles(%u)", TileTotal),
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, Shader, Params,
				FIntVector(FMath::DivideAndRoundUp(int32(TileTotal), FVoxelMarchCompactCS::kGroupSize),
				           1, 1));
		}

		// ---- THE DEPTH PRE-EMIT ------------------------------------------
		//
		// Writes SceneDepth HERE, before the base pass, because a raster pass at
		// PostRenderBasePassDeferred lands its colour and DROPS its depth: with
		// a full prepass the base pass holds depth DepthRead_StencilWrite
		// (DeferredShadingRenderer.cpp:2110-2113). Proven by forcing the emit to
		// OutDepth = 0.9 -- a value nothing can paint over -- and still getting a
		// sky-covered frame. The GBuffer emit later writes colour into the pixels
		// this pass has already claimed.
		//
		// Mode 2 must NOT do this. Mode 2's whole contract is that the raster
		// path keeps the real targets and the marcher touches nothing the image
		// depends on; writing real depth there would corrupt the control arm and
		// silently invalidate every gate that runs in it.
		//
		// WHERE ITS TIME LANDS, because someone will ask and the perf line does
		// not say. This pass is INSIDE the March bracket (opened above, closed
		// after the gate passes), so it is folded into marchMs and there is no
		// depthPreEmitMs anywhere. The only per-pass reading available today is
		// ProfileGPU, which sees it by the RDG_EVENT_NAME below. Adding a fourth
		// timing ring would mean a field in FVoxelMarchState (VoxelMarchRenderer.h)
		// -- worth doing if this pass ever becomes interesting, and it is not
		// yet: it rasterises the SAME tile list as the emit with a strictly
		// cheaper pixel shader (a Load, a ray rebuild, one matrix multiply, and
		// no MRT), so it is bounded above by emitMs, which measures 0.12-0.29 ms
		// on mode=scene legs. That bound is what settled the conservative-depth
		// question in VoxelMarch.usf: there is under half a millisecond in both
		// raster passes combined, and the early-Z share of it is zero.
		if (Arm.Mode == 1)
		{
			auto* Params = GraphBuilder.AllocParameters<FVoxelMarchDepthOnlyParameters>();
			// FROM THE VIEW ITSELF, not from the stash's raw FRHIUniformBuffer*
			// (header:1321). Wrapping that raw pointer in a TUniformBufferRef
			// COMPILES AND PRODUCES AN UNSET BINDING -- the parameter struct tracks
			// "was this assigned", and the engine fataled with
			// "required shader parameter ... View was not set".
			Params->View = Entry.ViewKey->ViewUniformBuffer;
			Params->MarchView = MarchView;
			Params->MarchVis = Vis;
			Params->MarchHitT = HitT;
			Params->MarchTileList = GraphBuilder.CreateSRV(TileList, PF_R32_UINT);
			Params->MarchDrawArgs = DrawArgs;
			Params->RenderTargets.DepthStencil =
				FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad,
				                     ERenderTargetLoadAction::ELoad,
				                     FExclusiveDepthStencil::DepthWrite_StencilWrite);

			// THE SAME SIDE OF THE SWITCH AS THE MARCH THAT JUST RAN. The VS
			// needs it for the tile quad's size in screen pixels; the PS needs it
			// because it must run the SAME reconstruction the emit will run, or
			// the emit's equal-depth test fails against the depth written here
			// and terrain disappears in patches.
			FVoxelMarchDepthOnlyVS::FPermutationDomain DepthVSPermutation;
			DepthVSPermutation.Set<FVoxelMarchHalfResDim>(ResShift != 0);
			FVoxelMarchDepthOnlyPS::FPermutationDomain DepthPSPermutation;
			DepthPSPermutation.Set<FVoxelMarchHalfResDim>(ResShift != 0);
			TShaderMapRef<FVoxelMarchDepthOnlyVS> VertexShader(ShaderMap, DepthVSPermutation);
			TShaderMapRef<FVoxelMarchDepthOnlyPS> PixelShader(ShaderMap, DepthPSPermutation);
			const FIntRect DepthViewRect = Entry.ViewRect;
			FRDGBufferRef DepthDrawArgs = DrawArgs;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("VoxelMarch.DepthPreEmit"), Params,
				ERDGPassFlags::Raster | ERDGPassFlags::NeverCull,
				[Params, VertexShader, PixelShader, DepthViewRect,
				 DepthDrawArgs](FRHICommandList& RHICmdList)
				{
					FGraphicsPipelineStateInitializer PSOInit;
					RHICmdList.ApplyCachedRenderTargets(PSOInit);
					// No colour targets at all, so no blend state matters and
					// nothing here can disturb the GBuffer the base pass is about
					// to write.
					PSOInit.BlendState = TStaticBlendState<>::GetRHI();
					PSOInit.RasterizerState =
						TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
					// Depth WRITE, and the same CF_DepthNearOrEqual the emit uses
					// so the two agree about which surface wins. Stencil is left
					// alone here -- the emit still stamps the receive-decal bit.
					PSOInit.DepthStencilState =
						TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();
					PSOInit.BoundShaderState.VertexDeclarationRHI =
						GEmptyVertexDeclaration.VertexDeclarationRHI;
					PSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
					PSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
					PSOInit.PrimitiveType = PT_TriangleList;
					SetGraphicsPipelineState(RHICmdList, PSOInit, 0);
					SetShaderParameters(RHICmdList, VertexShader,
					                    VertexShader.GetVertexShader(), *Params);
					SetShaderParameters(RHICmdList, PixelShader,
					                    PixelShader.GetPixelShader(), *Params);
					RHICmdList.SetViewport(float(DepthViewRect.Min.X), float(DepthViewRect.Min.Y),
					                       0.0f, float(DepthViewRect.Max.X),
					                       float(DepthViewRect.Max.Y), 1.0f);
					RHICmdList.DrawPrimitiveIndirect(
						DepthDrawArgs->GetIndirectRHICallBuffer(), 0);
				});
		}

		// ---- the source comparator ---------------------------------------
		//
		// OUTSIDE THE TIMING BRACKET: it marches every ray THREE times (both
		// sources plus the noise control), so inside it the number the phase
		// exists to produce would be meaningless. Its own cvar and its own run.
		const int32 SettleFrames =
			FMath::Max(CVarVoxelMarchSettleFrames.GetValueOnRenderThread(), 0);
		// COMPLETENESS, NOT QUIET, IS THE REAL SIGNAL.
		//
		// A 60-frame upload-quiet window during streaming is a LULL. This
		// project's cold-fill tooling already records a leg that read a mid-fill
		// lull as convergence and made the GPU fork look 12% faster than it was.
		//
		// LIKE FOR LIKE, ON BOTH SIDES, AND IT WAS NOT.
		//
		// This used to compare the index's TOTAL entry count against the pool's
		// LEVEL-0 census, on the strength of a comment saying "the index tracks
		// LEVEL 0 ONLY". The index has been multi-level since the ring cascade
		// landed -- 98,343 entries against a level-0 pool census of 27,538 -- so
		// the equality could never hold and bIndexComplete was PERMANENTLY
		// FALSE. A settle gate that can never pass is not a gate.
		//
		// Both sides are now level 0, which is what the sentence above always
		// meant. The totals are still read, for the log line only.
		//
		// AND THE POOL SIDE IS READ ATOMICALLY, NOT WALKED. GetLevelCensus
		// iterates the resident map; called from the render thread while the
		// game thread streams, it tripped UE's own
		// "Container has changed during ranged-for iteration" check every leg.
		const int32 PoolLevel0Chunks =
			GetGlobalVoxelBrickPool().GetResidentChunkCountAtLevel(0);
		const int32 IndexLevel0Now = GetGlobalVoxelMarchChunkIndex().GetNumEntriesAtLevel(0);
		const int32 IndexEntriesNow = GetGlobalVoxelMarchChunkIndex().GetNumEntries();
		const bool bIndexComplete =
			(PoolLevel0Chunks > 0) && (IndexLevel0Now == PoolLevel0Chunks);
		// THE STREAMING SYSTEM'S OWN SIGNAL, which is the one that does not move.
		// -1 means the publisher has never been called; that is reported rather
		// than silently downgraded, because a gate that weakens without saying so
		// is exactly what produced eight rounds of unreadable numbers.
		const int32 ConvergedFrames = GStreamConvergedFrames.load();
		const bool bStreamWired = (ConvergedFrames >= 0);
		const bool bStreamConverged = bStreamWired && (ConvergedFrames >= SettleFrames);
		{
			FScopeLock Guard(&State->Lock);
			State->Stats.StreamingConvergedFrames = ConvergedFrames;
		}
		// COMPLETENESS IS ADVISORY, NOT BLOCKING, AND THE REASON IS A REFRAME.
		//
		// VerifySource is a SAME-FRAME comparison: both walks run in one kernel
		// invocation over the same rays against the same SRVs. So `disagree` is a
		// defect count valid in ITS OWN RUN, whatever any other run says.
		// Cross-run reproducibility is a prerequisite for ATTRIBUTING A FIX; it
		// is not a prerequisite for CERTIFYING CORRECTNESS, and conflating the
		// two sent this workstream after a convergence problem certification does
		// not require.
		//
		// Measured: in one leg the world converged at 04:32:24 and the comparator
		// sampled at 04:38:46 -- six minutes later -- and the hashes still
		// differed, because the brick volume itself is not reproducible run to
		// run (97,019 / 97,082 / 97,471 resident across identical legs). Gating
		// harder cannot fix that, and does not need to.
		//
		// So the quiet window stays -- it keeps the sample out of a storm, which
		// is cheap and sane -- and index completeness is REPORTED rather than
		// enforced.
		const bool bVolumeSettled = (State->FramesSinceIndexChanged >= SettleFrames) &&
		                            (!bStreamWired || bStreamConverged);
		if (!bIndexComplete && bVolumeSettled)
		{
			static uint64 LastIncompleteWarn = 0;
			if (GFrameNumberRenderThread - LastIncompleteWarn > 600u)
			{
				LastIncompleteWarn = GFrameNumberRenderThread;
				UE_LOG(LogVoxelMarch, Display,
				       TEXT("voxel.March.VerifySource: index holds %d of the pool's %d level-0 "
				            "chunks (%d index entries across all levels). NOT a blocker -- the "
				            "comparison is same-frame, so its disagreement count is valid over "
				            "whatever world this run holds. It does mean per-term counts are not "
				            "comparable against another run."),
				       IndexLevel0Now, PoolLevel0Chunks, IndexEntriesNow);
			}
		}
		if (Arm.Source == 1 && CVarVoxelMarchVerifySource.GetValueOnRenderThread() != 0 &&
		    !bVolumeSettled)
		{
			// LOUD, AND ONCE EVERY SECOND OR SO RATHER THAN EVERY FRAME. A gate
			// that silently declines is indistinguishable from one that ran.
			static uint64 LastComplaint = 0;
			if (GFrameNumberRenderThread - LastComplaint > 120u)
			{
				LastComplaint = GFrameNumberRenderThread;
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("voxel.March.VerifySource is WAITING: streaming converged for %d "
				            "frames%s; index has %d of the pool's %d "
				            "level-0 chunks; index quiet %d of the %d frames required. "
				            "Sampling now would measure the streaming schedule rather than the "
				            "walk -- an upload-quiet window during streaming is a LULL, not "
				            "convergence. Set voxel.March.SettleFrames 0 to sample anyway and own "
				            "the consequence."),
				       ConvergedFrames,
				       bStreamWired
				           ? TEXT("")
				           : TEXT(" (SIGNAL UNWIRED -- VoxelMarchPublishStreamingState is never "
				                  "called, so convergence is NOT being checked and this gate is "
				                  "weaker than it looks)"),
				       IndexLevel0Now, PoolLevel0Chunks, State->FramesSinceIndexChanged,
				       SettleFrames);
			}
		}
		// THE COMPARATOR MUST WALK WHAT THE MARCHER WALKS, OR IT DOES NOT RUN.
		//
		// Refused rather than caveated: this instrument's failure mode is FALSE
		// AGREEMENT -- both arms omitting the same layer and reporting that the
		// marcher is correct about a picture it is not drawing. A caveat on such
		// a number survives exactly until someone copies it into a document.
		//
		// AND-ed INTO THE CONDITION, NEVER AN EARLY RETURN, and that is not
		// style. voxel.March.VerifyIndex is dispatched further down THIS SAME
		// FUNCTION, so returning here would refuse one instrument by silently
		// disabling a different one -- the very failure this guard exists to
		// prevent, displaced by one probe. Evaluated only when the comparator was
		// actually asked for, so a leg running cover without the comparator does
		// not log about it every frame.
		bool bComparatorShapeOk = true;
		if (Arm.Source == 1 && CVarVoxelMarchVerifySource.GetValueOnRenderThread() != 0)
		{
			FString ShapeWhy;
			bComparatorShapeOk = VoxelMarchComparatorShapeAgrees(ShapeWhy);
			if (!bComparatorShapeOk)
			{
				static bool bShapeRefusalLogged = false;
				if (!bShapeRefusalLogged)
				{
					bShapeRefusalLogged = true;
					UE_LOG(LogVoxelMarch, Error,
					       TEXT("voxel.March.VerifySource: REFUSING TO RUN -- %s"), *ShapeWhy);
				}
			}
		}
		// Volume.IsValid() IS PART OF THE CONDITION, not an early return inside the
		// block: this is a plain block in the middle of the hook, so returning
		// would skip the depth pre-emit and the emit setup below it. The
		// comparator's occupancy half genuinely needs the volume, and bOccValid
		// already exists so a skipped row reads as "not run" rather than "ran and
		// found nothing wrong".
		if (Arm.Source == 1 && CVarVoxelMarchVerifySource.GetValueOnRenderThread() != 0 &&
		    Volume.IsValid() &&
		    bVolumeSettled && !State->bSourceCompareInFlight && bComparatorShapeOk)
		{
			auto* CParams = GraphBuilder.AllocParameters<FVoxelMarchVerifySourceParameters>();
			Volume->BindShaderParameters(GraphBuilder, *CParams);
			int32 IgnoredEntries = 0;
			if (VoxelMarchBindPool(GraphBuilder, 1, *CParams, IgnoredEntries))
			{
				FRDGBufferRef CCounters = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(
						sizeof(uint32), uint32(FVoxelMarchState::kSourceCompareWords)),
					TEXT("VoxelMarch.SourceCompare"));
				FRDGBufferUAVRef CUAV = GraphBuilder.CreateUAV(CCounters, PF_R32_UINT);
				AddClearUAVPass(GraphBuilder, CUAV, 0u);

				// THE SAME VIEW BLOCK THE MARCH USED, copied rather than rebuilt.
				// The comparator has to trace the ray the renderer traces, and a
				// second construction of "the same" ray is how two instruments
				// end up describing two populations.
				CParams->MarchView = MarchView;
				CParams->MarchStepBudget = Arm.StepBudget;
				CParams->MarchHasPrepassDepth = bDepthBufferIsPopulated ? 1 : 0;
				CParams->MarchSceneDepthTexture = SceneDepth;
				CParams->MarchBrickOriginVoxel = FrameOriginVoxel;
				CParams->MarchPackOriginVoxel = Entry.CameraVoxel;
				CParams->MarchOccValid = bFrameIsVolumeFrame ? 1 : 0;
				CParams->MarchMutateCounters =
					CVarVoxelMarchMutateCounters.GetValueOnRenderThread();
				CParams->MarchIndexResidency = GetGlobalVoxelMarchChunkIndex().GetNumEntries();
				CParams->MarchIndexUploads =
					int32(FMath::Min<uint64>(GetGlobalVoxelMarchChunkIndex().GetUploads(),
					                         uint32(MAX_int32)));
				{
					// THE COMPARATOR OWNS THE HASH, because it is the only thing
					// that reads it and it costs ~85% of the streaming tick to
					// maintain (a whole-grid FNV -- 56 MiB -- on the game thread,
					// once per flush). Switched on here rather than read from a
					// cvar inside the index, so the index keeps no opinion about
					// who wants it. Idempotent; the first frame this branch runs
					// still reports the previous hash, which is correct -- the
					// value describes the grid the LAST flush produced.
					GetGlobalVoxelMarchChunkIndex().SetContentHashEnabled(true);
					const uint64 IdxHash = GetGlobalVoxelMarchChunkIndex().GetContentHash();
					CParams->MarchIndexHash =
						FUintVector2(uint32(IdxHash & 0xFFFFFFFFull), uint32(IdxHash >> 32));
					CParams->MarchPoolLevel0 = PoolLevel0Chunks;
					// F: taken at DISPATCH. The readback lands frames later, so
					// anything sampled at print time describes a different frame
					// from the one that produced the counts.
					CParams->MarchFrameNumber = uint32(GFrameNumberRenderThread);
					CParams->MarchQuietFrames = State->FramesSinceIndexChanged;
				}
				CParams->MarchOutVerifySource = CUAV;

				// The comparator must be compiled for the arm the frame was BUILT
				// for. It is the same Arm.bRings the march CS uses; passing it
				// here is what stops the CPU's box and the shader's box being
				// different shapes.
				FVoxelMarchVerifySourceCS::FPermutationDomain CPermutation;
				CPermutation.Set<FVoxelMarchRingsDim>(Arm.bRings);
				// MUST MATCH the march CS (see FVoxelMarchWalkShape): a
				// comparator at a different fallthrough depth walks a
				// different cascade than the one drawn.
				CPermutation.Set<FVoxelMarchFallthroughDim>(Arm.Fallthrough);
				TShaderMapRef<FVoxelMarchVerifySourceCS> CShader(ShaderMap, CPermutation);
				// FULL-RES GRID, NOT THE MARCH'S. This kernel is one thread per
				// SCREEN pixel and compares populations; dispatched over the
				// half-res grid it would judge a quarter of the frame and print
				// rates that looked like whole-frame rates. It is unchanged at
				// full res -- FullResTileCount == TileCount there.
				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("VoxelMarch.VerifySource"),
					ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, CShader, CParams,
					FIntVector(FullResTileCount.X, FullResTileCount.Y, 1));

				if (!State->SourceCompareReadback.IsValid())
				{
					State->SourceCompareReadback = MakeUnique<FRHIGPUBufferReadback>(
						TEXT("VoxelMarch.SourceCompareReadback"));
				}
				AddEnqueueCopyPass(GraphBuilder, State->SourceCompareReadback.Get(), CCounters,
				                   uint32(FVoxelMarchState::kSourceCompareWords) * sizeof(uint32));
				// Carried with the readback, not re-derived at decode: the frame
				// can change between dispatch and landing, and a row decoded
				// against the wrong gate reads as "ran and found nothing".
				State->bSourceCompareOccValid = bFrameIsVolumeFrame;
				State->bSourceCompareArmRingsCpu = Arm.bRings;
				State->SourceCompareViewSize = Entry.ViewRect.Size();
				State->bSourceCompareInFlight = true;
			}
		}

		// ---- the index/record join probe -------------------------------
		//
		// OUTSIDE THE TIMING BRACKET, like every other diagnostic here: it is a
		// second traversal of the same data and would inflate the number the
		// phase exists to produce. Source 1 only -- there is no join to probe
		// when the marcher is reading a flat bitfield.
		if (Arm.Source == 1 && CVarVoxelMarchVerifyIndex.GetValueOnRenderThread() != 0 &&
		    Volume.IsValid() && !State->bIndexProbeInFlight)
		{
			auto* PParams = GraphBuilder.AllocParameters<FVoxelMarchVerifyIndexParameters>();
			Volume->BindShaderParameters(GraphBuilder, *PParams);
			int32 IgnoredEntries = 0;
			if (VoxelMarchBindPool(GraphBuilder, 1, *PParams, IgnoredEntries))
			{
				FRDGBufferRef PCounters = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(
						sizeof(uint32), uint32(FVoxelMarchState::kIndexProbeWords)),
					TEXT("VoxelMarch.IndexProbe"));
				FRDGBufferUAVRef PUAV = GraphBuilder.CreateUAV(PCounters, PF_R32_UINT);
				AddClearUAVPass(GraphBuilder, PUAV, 0u);
				PParams->MarchOutVerifyIndex = PUAV;
				// The chunk box the marcher can actually touch: the occupancy
				// window is 512 voxels across, i.e. 16 chunks, anchored at the
				// SAME origin the marcher uses. Probing anywhere else would be
				// asking about chunks no ray can reach.
				// THE MARCHER'S FRAME, not the occupancy volume's. Under
				// voxel.March.ReachM the two differ, and a probe anchored to the
				// wrong one would report cleanly about chunks no ray visits.
				const FIntVector OriginVoxel = FrameOriginVoxel;
				PParams->MarchVerifyIndexBaseChunk =
					FIntVector(OriginVoxel.X >> 5, OriginVoxel.Y >> 5, OriginVoxel.Z >> 5);
				// PHASE 6: ring 0 unless a leg asks for the cover grid. SET
				// EXPLICITLY rather than left to the struct's zero-init, because
				// "the default happens to be the value I want" is how a binding
				// stops being a decision.
				PParams->MarchVerifyIndexCover = 0;

				TShaderMapRef<FVoxelMarchVerifyIndexCS> PShader(ShaderMap);
				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("VoxelMarch.VerifyIndex"),
					ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, PShader, PParams,
					FIntVector(4, 4, 4));

				if (!State->IndexProbeReadback.IsValid())
				{
					State->IndexProbeReadback =
						MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelMarch.IndexProbeReadback"));
				}
				AddEnqueueCopyPass(GraphBuilder, State->IndexProbeReadback.Get(), PCounters,
				                   uint32(FVoxelMarchState::kIndexProbeWords) * sizeof(uint32));
				State->bIndexProbeInFlight = true;
			}
		}

		// Enqueue the hit-tile readback. One slot, four bytes; a frame that finds
		// none is counted rather than dropped silently.
		{
			FVoxelMarchState::FTileReadback* Free = nullptr;
			for (FVoxelMarchState::FTileReadback& Slot : State->TileRing)
			{
				if (!Slot.bInFlight) { Free = &Slot; break; }
			}
			if (Free == nullptr)
			{
				State->TileReadbackSkips++;
			}
			else
			{
				if (!Free->Readback.IsValid())
				{
					Free->Readback =
						MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelMarch.TileCountReadback"));
				}
				AddEnqueueCopyPass(GraphBuilder, Free->Readback.Get(), DrawArgs,
				                   4 * sizeof(uint32));
				Free->bInFlight = true;
			}
		}

		Entry.VisBuffer = Vis;
		Entry.HitDistance = HitT;
		Entry.EmitTileList = TileList;
		Entry.EmitDrawArgs = DrawArgs;
		Entry.bMarched = true;
	}

	CloseBracket(GraphBuilder, Timing, TEXT("March"));

	VoxelRenderFrame::NoteMarchTiles(TotalTiles);

	FScopeLock Guard(&State->Lock);
	State->Stats.Frames++;
	State->Stats.Mode = Arm.Mode;
	State->Stats.TilesTotal = TotalTiles;
}

// ---------------------------------------------------------------------------
// HOOK 3 -- emit
// ---------------------------------------------------------------------------
void FVoxelMarchRenderExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneView& InView,
	const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	VOXEL_RENDER_FRAME_SCOPE(MarchEmit);
	const FVoxelMarchArm Arm = VoxelMarchGetArm();
	if (Arm.Mode == 0)
	{
		return;
	}
	FViewMarch* Entry = FindView(&InView);
	if (Entry == nullptr || !Entry->bMarched || Entry->FrameNumber != GFrameNumberRenderThread)
	{
		FScopeLock Guard(&State->Lock);
		State->Stats.DeclinedNoView++;
		return;
	}

	// ---- HALF RES: TAKEN FROM THE BUFFER, NOT FROM THE CVAR ----------------
	//
	// This hook runs LATER IN THE FRAME than the march. Re-reading
	// voxel.March.HalfRes here would let a mid-frame flip give the emit a
	// different answer from the march that produced the buffer it is about to
	// read -- and the symptom would not be a flicker, it would be a pixel shader
	// indexing a texture at twice its extent. The march's own output settles it:
	// if the VisBuffer is not the size of the view rect, the march ran half res.
	//
	// AND IT IS CHECKED, NOT MERELY DERIVED. Five bugs in this project in three
	// days shared one shape -- a join COMPUTED instead of CHECKED -- so the
	// derived answer is validated against the arithmetic it should satisfy, and
	// a mismatch is loud rather than silently plausible.
	const FIntPoint EmitViewSize = Entry->ViewRect.Size();
	const FIntPoint EmitVisExtent =
		(Entry->VisBuffer != nullptr) ? Entry->VisBuffer->Desc.Extent : EmitViewSize;
	const bool bHalfResEmit =
		(EmitVisExtent.X != EmitViewSize.X) || (EmitVisExtent.Y != EmitViewSize.Y);
	if (bHalfResEmit)
	{
		const FIntPoint Expected(FMath::DivideAndRoundUp(EmitViewSize.X, 2),
		                         FMath::DivideAndRoundUp(EmitViewSize.Y, 2));
		if (EmitVisExtent != Expected)
		{
			static bool bComplained = false;
			if (!bComplained)
			{
				bComplained = true;
				UE_LOG(LogVoxelMarch, Error,
				       TEXT("Voxel march emit: the VisBuffer is %dx%d for a %dx%d view rect, "
				            "which is neither full res nor the %dx%d half-res grid the march is "
				            "supposed to produce. The emit is about to reconstruct from a buffer "
				            "whose sample lattice it does not know, which draws terrain in the "
				            "wrong place with no other symptom. Something other than "
				            "voxel.March.HalfRes changed the march's output extent."),
				       EmitVisExtent.X, EmitVisExtent.Y, EmitViewSize.X, EmitViewSize.Y,
				       Expected.X, Expected.Y);
			}
		}
	}

	TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe> Volume;
	{
		FScopeLock Guard(&State->Lock);
		Volume = State->Volume;
	}
	// SOURCE 0 STILL REQUIRES IT; source 1 never traverses the volume and must
	// not decline for its absence. Registering is still attempted when there IS
	// one, because a volume that exists but has never run AddPasses has no
	// buffer and binding it would assert.
	if (VoxelMarchNeedsOccupancyVolume(VoxelMarchGetArm()) &&
	    (!Volume.IsValid() || Volume->Register(GraphBuilder) == nullptr))
	{
		FScopeLock Guard(&State->Lock);
		State->Stats.DeclinedNoVolume++;
		return;
	}
	if (Volume.IsValid() && Volume->Register(GraphBuilder) == nullptr)
	{
		// Present but unbuilt. Drop it rather than bind a bufferless volume; the
		// stand-in below is the safe binding and source 1 does not read it.
		Volume.Reset();
	}

	// ---- the targets -------------------------------------------------------
	//
	// WE DO NOT REUSE THE BASE PASS'S SLOT INDICES, and this is worth reading
	// before "simplifying" it back.
	//
	// The `RenderTargets` argument is the base pass's own FRenderTargetBindingSlots
	// at the base pass's own slot indices -- which are DATA-DRIVEN
	// (FSceneTextures::GetGBufferRenderTargets reads Config.GBufferBindings, so
	// GBufferA might be slot 1 or slot 4 depending on the layout the project
	// resolved). An HLSL SV_Target index is a compile-time constant, so binding
	// through those slots means a shader whose output semantics have to match a
	// runtime-decided layout, which cannot be written correctly and can only be
	// written plausibly.
	//
	// So we build OUR OWN slots at FIXED indices from the textures themselves,
	// which are public members of FSceneTextureUniformParameters:
	//
	//     0 SceneColor  1 GBufferA  2 GBufferB  3 GBufferC  4 GBufferD
	//     5 GBufferE    6 Velocity
	//
	// We are not participating in the base pass's MRT set; we are a later pass
	// writing the same textures. The textures are the ones deferred lighting
	// reads, which is the only property that has to hold.
	//
	// The argument is still used -- for its DepthStencil texture, which is the
	// scene depth this emit has to win against and then write.
	// ---- WHERE THE TARGETS COME FROM, AND THE BUG THAT MADE THIS COMMENT ----
	//
	// THEY COME FROM THE HOOK'S OWN `RenderTargets` ARGUMENT. They used to come
	// from FSceneTextureUniformParameters, and that was WRONG IN A WAY THAT DID
	// NOT ERROR FOR A WHOLE LEG.
	//
	// UE::FXRenderingUtils::GetOrCreateSceneTextureUniformBuffer IGNORES the
	// SetupMode you pass whenever the frame already has scene textures -- it
	// returns SceneTextures->UniformBuffer verbatim (FXRenderingUtils.cpp:
	// 109-112) and only honours SetupMode on the fallback path at :118. And the
	// renderer sets SetupMode = ESceneTextureSetupMode::SceneDepth during the
	// prepass (DeferredShadingRenderer.cpp:2499) and does not add SceneColor or
	// the GBuffers until later in the frame.
	//
	// So at THIS hook that uniform buffer hands back the 1x1 black system
	// texture for SceneColor and for every GBuffer. The emit bound 1x1 dummies
	// as its MRTs and reported a perfectly plausible 0.074 ms for it. The only
	// reason it was caught at all is that mode 2 copies the descriptor to build
	// its scratch set, and RDG then refused to clear a 1x1 texture that had no
	// TexCreate_RenderTargetable -- an error about the SCRATCH copy that was
	// really about the SOURCE.
	//
	// The `RenderTargets` argument cannot have this problem: it is the exact
	// binding set the base pass just finished drawing through.
	//
	// WHICH LEAVES THE SLOT-INDEX QUESTION THIS FILE ALREADY ARGUED ABOUT. The
	// engine's indices are data-driven, an SV_Target index is compile-time, so
	// the shader keeps ITS OWN fixed order and the mapping is done here, on the
	// CPU, from the same table the renderer built the bindings from:
	// FSceneTexturesConfig::Get().GBufferBindings[GBL_Default]. Public, and the
	// single source of truth for which engine slot holds which GBuffer.
	const FGBufferBindings& Bindings =
		FSceneTexturesConfig::Get().GBufferBindings[GBL_Default];

	const auto FromSlot = [&RenderTargets](int32 Index) -> FRDGTextureRef
	{
		// Index <= 0 means "not in this layout" for everything except scene
		// colour, which is always slot 0 (FSceneTextures::GetGBufferRenderTargets:
		// "All configurations use scene color in the first slot").
		return Index > 0 ? RenderTargets[uint32(Index)].GetTexture() : nullptr;
	};

	FRDGTextureRef SceneDepth = RenderTargets.DepthStencil.GetTexture();
	FRDGTextureRef Targets[7] = {
		RenderTargets[0].GetTexture(),
		FromSlot(Bindings.GBufferA.Index),
		FromSlot(Bindings.GBufferB.Index),
		FromSlot(Bindings.GBufferC.Index),
		FromSlot(Bindings.GBufferD.Index),
		FromSlot(Bindings.GBufferE.Index),
		// Velocity is either IN the base pass layout (r.VelocityOutputPass 1) or
		// a separate texture written by its own pass. Both are handled, and the
		// separate one is the default here.
		Bindings.GBufferVelocity.Index > 0
			? FromSlot(Bindings.GBufferVelocity.Index)
			: UE::FXRenderingUtils::GetSceneVelocityTexture(InView),
	};

	if (SceneDepth == nullptr || Targets[0] == nullptr || Targets[1] == nullptr ||
	    Targets[2] == nullptr || Targets[3] == nullptr)
	{
		// GBufferD/E can legitimately be absent (a project with no precomputed
		// shadows and no custom-data shading models drops them from the layout);
		// SceneColor, A, B and C cannot, and their absence means this is not the
		// deferred path we think it is.
		FScopeLock Guard(&State->Lock);
		State->Stats.DeclinedNoTextures++;
		return;
	}

	// THE ONE THING THE SHADER AND THE HOST MUST AGREE ON, CHECKED RATHER THAN
	// ASSUMED. The pixel shader's SV_Target list branches on
	// ALLOW_STATIC_LIGHTING (a global compile define, set from
	// IsStaticLightingAllowed in ShaderCompiler.cpp:3893); the binding slots
	// below branch on whether GBufferE exists. Those are the same question asked
	// two ways, and if they ever answer differently the velocity target lands one
	// slot away from where the shader writes it -- which produces motion vectors
	// in GBufferE and nothing in velocity, i.e. a lit frame with subtly wrong
	// shadow factors and a smearing far field, and no error anywhere.
	//
	// Declined loudly instead. This has never fired; it exists so that a project
	// setting change cannot make it fire silently.
	if (IsStaticLightingAllowed() != (Targets[5] != nullptr))
	{
		static bool bComplained = false;
		if (!bComplained)
		{
			bComplained = true;
			UE_LOG(LogVoxelMarch, Error,
			       TEXT("voxel.March declined: IsStaticLightingAllowed()=%d but GBufferE is %s. "
			            "The emit's SV_Target layout is compiled against ALLOW_STATIC_LIGHTING and "
			            "would bind velocity to the wrong slot. Fix the project's static lighting "
			            "setting or the shader's slot branch -- do not just delete this check."),
			       IsStaticLightingAllowed() ? 1 : 0,
			       Targets[5] != nullptr ? TEXT("present") : TEXT("absent"));
		}
		FScopeLock Guard(&State->Lock);
		State->Stats.DeclinedUnsupported++;
		return;
	}

	// ---- mode 2: everything above, to scratch ------------------------------
	//
	// A full scratch copy, descriptor for descriptor, so the marcher runs the
	// SAME shader against the SAME formats and pays the same bandwidth -- and
	// the frame on screen stays the control frame. A cheaper scratch (one RGBA
	// target, no depth) would make mode 2 measure a pass that does not exist.
	//
	// ONE SEMANTIC DIFFERENCE, AND IT IS THE RIGHT ONE FOR THE GATE. The scratch
	// depth is CLEARED, not copied from the real scene depth -- depth copies are
	// restricted and a raster copy would cost more than the pass being measured.
	// So it starts at the far plane and the marcher's DepthNearOrEqual test wins
	// everywhere, i.e. mode 2 draws terrain that is NOT occluded by trees,
	// characters or water, while mode 1's is.
	//
	// For the depth gate that is what you want: gate 3 asks whether the marched
	// surface lands where PrePass + BasePass put the terrain, and an occlusion
	// mask applied to one side and not the other would make every occluded pixel
	// read as a depth disagreement. For a COLOUR gate it is a difference that
	// has to be masked out. Said here because the two gates read the same buffer
	// and only one of them can take it at face value.
	//
	// (The march itself still clips t_max against the REAL prepass depth in both
	// modes -- that happens at the earlier hook, before any of this. So mode 2's
	// marcher does not do more work than mode 1's; it only escapes the hardware
	// depth test at emit time.)
	const bool bScratch = (Arm.Mode == 2);
	// THE HTILE PROBE. Mode 2 only, and it changes exactly one binding: the
	// emit's depth-stencil target goes to the REAL SceneDepth while every colour
	// target still goes to scratch. That makes probe-on minus probe-off differ
	// ONLY in whether the real depth buffer's HTILE metadata survived the frame
	// -- which is the downstream decompression bill, isolated, and the only way
	// to see it from outside the renderer. See the cvar text for why mode 2
	// without it necessarily measures that bill as zero.
	const bool bHTileProbe = bScratch && CVarVoxelMarchHTileProbe.GetValueOnRenderThread() != 0;
	FRDGTextureRef RealSceneDepth = SceneDepth;

	// The scratch set costs seven full-screen clears the control frame does not
	// pay. Bracketed on its own so a mode-2 frame-time delta can be decomposed
	// rather than attributed wholesale to the marcher.
	FVoxelMarchState::FTimingPair* ScratchTiming =
		bScratch ? OpenBracket(GraphBuilder, State->ScratchTiming, TEXT("Scratch")) : nullptr;
	if (bScratch)
	{
		const auto Scratch = [&GraphBuilder](FRDGTextureRef Src, const TCHAR* Name) -> FRDGTextureRef
		{
			if (Src == nullptr)
			{
				return nullptr;
			}
			FRDGTextureDesc Desc = Src->Desc;
			FRDGTextureRef Out = GraphBuilder.CreateTexture(Desc, Name);
			// Cleared, not left undefined: an uncleared scratch target makes a
			// wrong write and a missing write look identical.
			// TexCreate_DepthStencilTargetable, not TexCreate_DepthStencil: the
			// latter does not exist. In UE 5.8 the flags are ETextureCreateFlags
			// and the TexCreate_* names are #defines onto it
			// (RHIDefinitions.h:1191); DepthStencilTargetable (bit 2) is the one
			// that says "this can be bound as a depth-stencil target", which is
			// the question being asked here -- clear it as depth, or as colour.
			if (EnumHasAnyFlags(Desc.Flags, TexCreate_DepthStencilTargetable))
			{
				AddClearDepthStencilPass(GraphBuilder, Out, ERenderTargetLoadAction::EClear,
				                         ERenderTargetLoadAction::EClear);
			}
			else
			{
				AddClearRenderTargetPass(GraphBuilder, Out);
			}
			return Out;
		};
		SceneDepth = Scratch(SceneDepth, TEXT("VoxelMarch.Scratch.Depth"));
		Targets[0] = Scratch(Targets[0], TEXT("VoxelMarch.Scratch.SceneColor"));
		Targets[1] = Scratch(Targets[1], TEXT("VoxelMarch.Scratch.GBufferA"));
		Targets[2] = Scratch(Targets[2], TEXT("VoxelMarch.Scratch.GBufferB"));
		Targets[3] = Scratch(Targets[3], TEXT("VoxelMarch.Scratch.GBufferC"));
		Targets[4] = Scratch(Targets[4], TEXT("VoxelMarch.Scratch.GBufferD"));
		Targets[5] = Scratch(Targets[5], TEXT("VoxelMarch.Scratch.GBufferE"));
		Targets[6] = Scratch(Targets[6], TEXT("VoxelMarch.Scratch.Velocity"));
		if (bHTileProbe)
		{
			// Put the depth back. Everything else stays scratch.
			SceneDepth = RealSceneDepth;
		}
	}
	else if (Targets[6] != nullptr && !HasBeenProduced(Targets[6]))
	{
		// VELOCITY, AND THE HALF OF THE RULE THE PLAN DOES NOT STATE.
		//
		// The instruction is "do not clear it" -- correct, because HISM foliage,
		// characters and the water have already written theirs. But the engine's
		// own opaque velocity pass decides its load action with
		//     bNeedsClearMask = HasBeenProduced(TargetVelocity) ? 0 : ...
		// (VelocityRendering.cpp:505, used at :690). So by PRODUCING the texture
		// here we SUPPRESS the engine's clear, and every pixel neither we nor a
		// moving primitive writes is then left holding whatever was in that
		// allocation.
		//
		// With Nanite in the frame this never bites, because Nanite's own
		// EmitDepthTargets clears velocity before the base pass. Without it, it
		// bites silently and shows up as far-field TSR smear at a pose nobody
		// associates with the terrain renderer.
		//
		// So: clear it ourselves, exactly once, and only if nobody has produced
		// it yet -- which reproduces what the engine would have done and leaves
		// the "do not clear" rule intact for every frame where it matters.
		AddClearRenderTargetPass(GraphBuilder, Targets[6]);
	}

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelMarchEmit, "VoxelMarchEmit");

	CloseBracket(GraphBuilder, ScratchTiming, TEXT("Scratch"));

	// ---- WHAT THE EMIT IS ACTUALLY BOUND TO, LOGGED ONCE --------------------
	//
	// This exists because emitMs read 0.074 ms while the emit was writing 1x1
	// black system textures, and read 0.068 ms after that was fixed. The numbers
	// being nearly identical before and after a fix that changed what was being
	// written is the kind of coincidence that has to be checked rather than
	// explained -- so the binding is now stated in the log rather than inferred
	// from a timing.
	//
	// (The explanation, once the extents confirm it, is that this pass is
	// overhead-bound and not bandwidth-bound: ~8,200 tiles is ~528k pixels and
	// about 17 MB of MRT writes, which is well under 0.05 ms of bandwidth. The
	// 1x1 version was paying the same PSO set, the same indirect draw and the
	// same ROP setup and almost no bandwidth, so the two land in the same place.
	// That is a real result about the emit being cheap -- but only once the
	// extents below say it was writing the real thing.)
	{
		static bool bLoggedTargets = false;
		if (!bLoggedTargets)
		{
			bLoggedTargets = true;
			const TCHAR* Names[7] = { TEXT("SceneColor"), TEXT("GBufferA"), TEXT("GBufferB"),
			                          TEXT("GBufferC"), TEXT("GBufferD"), TEXT("GBufferE"),
			                          TEXT("Velocity") };
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("[voxel-march] emit halfres: %s -- VisBuffer %dx%d for a %dx%d view "
			            "rect, tile %d full-res px, reconstruction %s"),
			       bHalfResEmit ? TEXT("HALF RES") : TEXT("full res"), EmitVisExtent.X,
			       EmitVisExtent.Y, EmitViewSize.X, EmitViewSize.Y,
			       kVoxelMarchTileSize << (bHalfResEmit ? 1 : 0),
			       bHalfResEmit ? TEXT("ON (exact ray/plane over the 2x2, nearest-neighbour "
			                           "fallback)")
			                    : TEXT("off (direct Load)"));
			UE_LOG(LogVoxelMarch, Display,
			       TEXT("Voxel march emit bindings (mode %d, scratch %d): depth %s %dx%d"),
			       Arm.Mode, bScratch ? 1 : 0,
			       SceneDepth ? SceneDepth->Name : TEXT("NULL"),
			       SceneDepth ? SceneDepth->Desc.Extent.X : 0,
			       SceneDepth ? SceneDepth->Desc.Extent.Y : 0);
			for (int32 i = 0; i < 7; ++i)
			{
				UE_LOG(LogVoxelMarch, Display, TEXT("  slot %d %-11s -> %s %dx%d fmt %d"), i,
				       Names[i], Targets[i] ? Targets[i]->Name : TEXT("(unbound)"),
				       Targets[i] ? Targets[i]->Desc.Extent.X : 0,
				       Targets[i] ? Targets[i]->Desc.Extent.Y : 0,
				       Targets[i] ? int32(Targets[i]->Desc.Format) : 0);
			}
			// A 1x1 target is the dummy-system-texture failure this file already
			// shipped once. Loud, once, rather than a number nobody questions.
			for (int32 i = 0; i < 7; ++i)
			{
				if (Targets[i] != nullptr && Targets[i]->Desc.Extent.X <= 1)
				{
					UE_LOG(LogVoxelMarch, Error,
					       TEXT("  slot %d (%s) IS 1x1 -- that is a dummy system texture, not a "
					            "real target. Any emit timing from this run is meaningless."),
					       i, Names[i]);
				}
			}
		}
	}

	FVoxelMarchState::FTimingPair* Timing = OpenBracket(GraphBuilder, State->EmitTiming, TEXT("Emit"));

	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelMarchEmitParameters>();
		Params->View = InView.ViewUniformBuffer;
		// BOUND EVEN WITH NO VOLUME, and THIS is the pass where it matters: the
		// emit is added with a raw AddPass, so nothing calls
		// ClearUnusedGraphResources and an absent SRV is an RDG assert rather
		// than a black frame. The stand-in is filled SOLID and is reachable only
		// by source-1 permutations, whose shader maps do not carry these members.
		if (Volume.IsValid())
		{
			Volume->BindShaderParameters(GraphBuilder, *Params);
		}
		else
		{
			FVoxelFluidOccupancyVolume::BindNullShaderParameters(GraphBuilder, *Params);
		}
		Params->MarchView.MarchViewToTranslatedWorld = Entry->ViewToTranslatedWorld;
		Params->MarchView.MarchRayOriginLocalUU = Entry->FrameRayOriginLocalUU;
		Params->MarchView.MarchVolumeExtentUU = Entry->FrameExtentUU;
		Params->MarchView.MarchViewRectMin = FVector2f(Entry->ViewRect.Min);
		Params->MarchView.MarchViewRectSize = FVector2f(Entry->ViewRect.Size());
		Params->MarchView.MarchInvProjDiag = Entry->InvProjDiag;
		Params->MarchView.MarchTemporalAAJitter = Entry->TemporalAAJitter;
		Params->MarchView.MarchInvDeviceZToWorldZ = Entry->InvDeviceZToWorldZ;
		Params->MarchView.MarchPixelConeSlope = Entry->PixelConeSlope;
		Params->MarchView.MarchClimateStrength = CVarVoxelMarchClimateStrength.GetValueOnRenderThread();
		// TAKEN FROM THE MARCH'S OWN STAMP, not re-read from the cvar -- the
		// same argument bHalfResEmit above makes about the VisBuffer extent,
		// applied to the sample point inside it. bHalfResEmit gates it, so a
		// full-res frame gets zero however the cvar is set.
		Params->MarchView.MarchSampleJitter =
			VoxelMarchJitterForEmit(bHalfResEmit, Entry->FrameNumber);
		Params->MarchView.MarchTileCount = Entry->TileCount;
		Params->MarchStepBudget = Arm.StepBudget;
		Params->MarchBrickOriginVoxel = Entry->FrameOriginVoxel;
		// The emit unpacks against the SAME origin the march packed against.
		Params->MarchPackOriginVoxel = Entry->CameraVoxel;
		{
			// The emit reads only the VisBuffer, but it calls
			// VoxelMarchSourceOriginVoxel() to turn a packed LOCAL voxel back
			// into the WORLD voxel the palette hashes on -- so it compiles
			// against the source and its bindings have to be filled. A failure
			// here cannot happen (the march already succeeded this frame with
			// the same bindings) and is not treated as one.
			int32 IgnoredEntries = 0;
			VoxelMarchBindPool(GraphBuilder, Arm.Source, *Params, IgnoredEntries);

			// VOXEL GI. The quad path binds this per draw in the vertex factory;
			// the marcher binds it once for the emit. Null when GI has never
			// published -- the shader's Enabled check then reads 0 and the sample
			// is skipped, which is byte-identical to the behaviour before this.
			Params->VoxelGIVol = GVoxelGIVolume.GetUniformBufferRef();

			// THE INDEX MAY LEGITIMATELY BE UNAVAILABLE HERE, AND ONLY HERE.
			//
			// FVoxelMarchChunkIndex::Register hands back the staged buffer ONCE
			// and clears bStagedValid; the march pass, running earlier in the
			// frame, is the one that consumes it. The pooled copy it queues for
			// extraction is not valid until that graph has executed, so this
			// second Register in the same frame can return nullptr and leave
			// MarchChunkIndex UNSET.
			//
			// That was harmless until Wave 2, because the emit shader never
			// referenced the index -- an unset binding for an unreferenced
			// parameter is not required. The surface-proximity gate references
			// it, so UE now fatals with "required shader parameter
			// MarchChunkIndex was not set".
			//
			// A ZERO INDEX IS SAFE IN THIS PASS AND NOWHERE ELSE. Register's own
			// comment warns that binding null reads as zeros, zero means "not
			// resident", and a march against that draws an EMPTY WORLD with no
			// error. True -- for the march, which traverses the index. The emit
			// uses it for one thing: looking up the hit chunk's fitted surface
			// plane. A miss there leaves SurfaceBandFade at 1.0, which is
			// exactly the appearance before this wave. So the fallback is
			// fail-open in the direction that preserves the picture, and it is
			// applied HERE rather than inside VoxelMarchBindPool precisely so
			// the march keeps fataling instead of quietly emptying.
			if (Params->MarchChunkIndex == nullptr)
			{
				Params->MarchChunkIndex = GraphBuilder.CreateSRV(
					GSystemTextures.GetDefaultBuffer(GraphBuilder, sizeof(uint32), 0u),
					PF_R32_UINT);
			}
		}
		Params->MarchVis = Entry->VisBuffer;
		Params->MarchHitT = Entry->HitDistance;
		Params->MarchTileList = GraphBuilder.CreateSRV(Entry->EmitTileList, PF_R32_UINT);
		Params->MarchDrawArgs = Entry->EmitDrawArgs;

		// THE SLOT ARRAY MUST BE CONTIGUOUS, and this is the whole reason the
		// velocity slot is conditional rather than always 6.
		//
		// FRenderTargetBindingSlots::Enumerate and GetActiveCount both STOP AT
		// THE FIRST NULL (ShaderParameterMacros.h:726-753, :756-761). So if
		// GBufferE is absent -- which happens when static lighting is off -- a
		// velocity binding left at slot 6 sits behind a hole at slot 5 and is
		// silently never bound. No error, no warning: motion vectors simply stop
		// existing for terrain, and the symptom is TSR smear at a pose nobody
		// connects to a render-target index.
		//
		// So the slot moves, on BOTH SIDES, off the SAME predicate: the shader
		// switches on ALLOW_STATIC_LIGHTING (a global compile define set from
		// IsStaticLightingAllowed) and this switches on the texture's presence.
		// The two are checked against each other below rather than assumed to
		// agree.
		const bool bHasGBufferE = (Targets[5] != nullptr);
		const int32 VelocitySlot = bHasGBufferE ? 6 : 5;
		for (int32 i = 0; i < 6; ++i)
		{
			if (Targets[i] != nullptr)
			{
				Params->RenderTargets[i] = FRenderTargetBinding(Targets[i],
				                                               ERenderTargetLoadAction::ELoad);
			}
		}
		const bool bWriteVelocity = Arm.bVelocity && Targets[6] != nullptr;
		if (bWriteVelocity)
		{
			Params->RenderTargets[VelocitySlot] =
				FRenderTargetBinding(Targets[6], ERenderTargetLoadAction::ELoad);
		}
		// THE ONE BINDING THE BASE PASS COULD NOT HAVE GIVEN US.
		// DepthWrite_StencilWrite, on the same texture the base pass held as
		// DepthRead_StencilWrite, in a later pass. This is legal and it is
		// literally what Nanite::EmitDepthTargets does
		// (NaniteComposition.cpp:406) -- the base pass's exclusive state
		// constrains the base pass and nothing after it.
		Params->RenderTargets.DepthStencil =
			FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad,
			                     ERenderTargetLoadAction::ELoad,
			                     FExclusiveDepthStencil::DepthWrite_StencilWrite);

		FVoxelMarchEmitPS::FPermutationDomain Permutation;
		Permutation.Set<FVoxelMarchSourceDim>(Arm.Source);
		Permutation.Set<FVoxelMarchAODim>(Arm.bAO);
		Permutation.Set<FVoxelMarchDBufferDim>(Arm.bDBuffer);
		Permutation.Set<FVoxelMarchVelocityDim>(bWriteVelocity);
		Permutation.Set<FVoxelMarchRingsDim>(Arm.bRings);
		// FROM THE BUFFER, not from the cvar -- see the derivation at the top of
		// this hook. The VS carries it too: the tile quad's size in screen pixels
		// is a compile-time constant and a VS on the wrong side of the switch
		// would cover a quarter of its tile.
		Permutation.Set<FVoxelMarchHalfResDim>(bHalfResEmit);

		FVoxelMarchEmitVS::FPermutationDomain VSPermutation;
		VSPermutation.Set<FVoxelMarchHalfResDim>(bHalfResEmit);

		TShaderMapRef<FVoxelMarchEmitVS> VertexShader(ShaderMap, VSPermutation);
		TShaderMapRef<FVoxelMarchEmitPS> PixelShader(ShaderMap, Permutation);
		const FIntRect ViewRect = Entry->ViewRect;
		FRDGBufferRef DrawArgs = Entry->EmitDrawArgs;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VoxelMarch.Emit(%s)", bScratch ? TEXT("scratch") : TEXT("scene")),
			// NeverCull: in mode 2 nothing reads the scratch targets, and an emit
			// that RDG removed would report a near-zero emitMs for a pass that
			// never rasterised -- which is the number the 0.2-0.4 ms HTILE budget
			// is supposed to be checked against.
			Params, ERDGPassFlags::Raster | ERDGPassFlags::NeverCull,
			[Params, VertexShader, PixelShader, ViewRect, DrawArgs](FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer PSOInit;
				RHICmdList.ApplyCachedRenderTargets(PSOInit);
				// Opaque. No blending anywhere: every channel this shader touches
				// is a REPLACEMENT of what the base pass put there, and a blend
				// on a GBuffer is not a darker surface, it is a corrupt one.
				PSOInit.BlendState = TStaticBlendState<>::GetRHI();
				PSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				// Copied from Nanite's non-compute emit, term for term:
				// depth WRITE, CF_DepthNearOrEqual, stencil always-pass with
				// SO_Replace, ref = the receive-decal bit.
				//
				// NearOrEqual and not Greater is load-bearing. Two point samples
				// of the same step function agree at a shared surface to within
				// float error, so a strict test accepts about half of them and
				// the result reads as z-fighting on ground that is in fact
				// identical -- the same phenomenon VoxelMarchSpike.usf's DEPTH
				// SLACK note describes from the other side.
				PSOInit.DepthStencilState =
					TStaticDepthStencilState<true, CF_DepthNearOrEqual, true, CF_Always, SO_Keep,
					                         SO_Keep, SO_Replace>::GetRHI();
				PSOInit.BoundShaderState.VertexDeclarationRHI =
					GEmptyVertexDeclaration.VertexDeclarationRHI;
				PSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				PSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				PSOInit.PrimitiveType = PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, PSOInit, kVoxelMarchStencilReceiveDecalMask);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(),
				                    *Params);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Params);
				RHICmdList.SetViewport(float(ViewRect.Min.X), float(ViewRect.Min.Y), 0.0f,
				                       float(ViewRect.Max.X), float(ViewRect.Max.Y), 1.0f);
				// INDIRECT, over the tiles the march actually HIT. Six vertices
				// per instance, one instance per hit tile, instance count read
				// from the buffer the compact pass filled. Empty screen is never
				// rasterised, so it never pays the HTILE decompression that
				// SV_Depth forces on every tile it touches.
				RHICmdList.DrawPrimitiveIndirect(DrawArgs->GetIndirectRHICallBuffer(), 0);
			});
	}

	CloseBracket(GraphBuilder, Timing, TEXT("Emit"));

	// ---- the depth gate ----------------------------------------------------
	//
	// AFTER the timing bracket closed, deliberately and non-negotiably -- the
	// same rule the march spike's census follows. This pass reads every pixel a
	// second time; inside the bracket it would inflate the number the phase gate
	// exists to produce. It still makes the GPU busier, which is why it is its
	// own cvar and its own run, and why no timing from a gate run should be
	// quoted.
	//
	// AND IT IS REFUSED OUTSIDE MODE 2. In mode 1 the emit above has already
	// replaced SceneDepth with the marcher's own depth, so this would compare
	// the marcher against itself and report a flawless result. Refusing loudly
	// is the only acceptable behaviour: a gate that can be run in a
	// configuration where it cannot fail is worse than no gate.
	if (CVarVoxelMarchVerifyDepth.GetValueOnRenderThread() != 0)
	{
		// TWO WAYS THE GATE CAN BE ASKED TO GRADE ITS OWN WORK, AND BOTH ARE
		// REFUSED. Mode 1 overwrites SceneDepth with the marcher's depth; so does
		// mode 2 WITH THE HTILE PROBE, which is the whole point of the probe. In
		// either case the comparison would be the marcher against itself and
		// would pass no matter what was wrong. A gate that can be run in a
		// configuration where it cannot fail is worse than no gate.
		if (Arm.Mode != 2 || bHTileProbe)
		{
			static bool bComplained = false;
			if (!bComplained)
			{
				bComplained = true;
				UE_LOG(LogVoxelMarch, Warning,
				       TEXT("voxel.March.VerifyDepth 1 is being IGNORED (voxel.March=%d, "
				            "HTileProbe=%d). The gate needs mode 2 with the probe OFF: anything "
				            "that lets the emit write the real SceneDepth turns the comparison "
				            "into the marcher against itself, which passes no matter what. "
				            "Set voxel.March 2 and voxel.March.HTileProbe 0."),
				       Arm.Mode, bHTileProbe ? 1 : 0);
			}
		}
		else
		{
			// THE REAL SCENE DEPTH, taken from the hook's own argument rather
			// than from the local -- which by this point has been swapped to the
			// scratch copy. Comparing against the scratch would compare the
			// marcher with a cleared buffer and report every pixel as a
			// disagreement, which at least fails loudly; but it would fail for
			// the wrong reason and cost a leg to work out.
			FRDGTextureRef RasterDepth = RenderTargets.DepthStencil.GetTexture();
			FVoxelMarchState::FVerifyReadback* Free = nullptr;
			for (FVoxelMarchState::FVerifyReadback& Slot : State->VerifyRing)
			{
				if (!Slot.bInFlight) { Free = &Slot; break; }
			}
			if (RasterDepth != nullptr && Free != nullptr)
			{
				FRDGBufferRef Counters = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32),
					                                 uint32(FVoxelMarchState::kVerifyWords)),
					TEXT("VoxelMarch.VerifyCounters"));
				FRDGBufferUAVRef CountersUAV = GraphBuilder.CreateUAV(Counters, PF_R32_UINT);
				// Cleared every frame: the gate describes ONE frame's pixels.
				// Accumulating across frames would average over a drifting camera
				// and read as a much smoother agreement than any frame had.
				AddClearUAVPass(GraphBuilder, CountersUAV, 0u);

				auto* Params = GraphBuilder.AllocParameters<FVoxelMarchVerifyParameters>();
				Params->MarchView.MarchViewToTranslatedWorld = Entry->ViewToTranslatedWorld;
				Params->MarchView.MarchRayOriginLocalUU = Entry->FrameRayOriginLocalUU;
				Params->MarchView.MarchVolumeExtentUU = Entry->FrameExtentUU;
				Params->MarchView.MarchViewRectMin = FVector2f(Entry->ViewRect.Min);
				Params->MarchView.MarchViewRectSize = FVector2f(Entry->ViewRect.Size());
				Params->MarchView.MarchInvProjDiag = Entry->InvProjDiag;
				Params->MarchView.MarchTemporalAAJitter = Entry->TemporalAAJitter;
				Params->MarchView.MarchInvDeviceZToWorldZ = Entry->InvDeviceZToWorldZ;
				Params->MarchView.MarchPixelConeSlope = Entry->PixelConeSlope;
		Params->MarchView.MarchClimateStrength = CVarVoxelMarchClimateStrength.GetValueOnRenderThread();
				// THE GATE MUST STAND WHERE THE EMIT STANDS. It grades the depth
				// the emit would have written, so it goes through the same
				// reconstruction over the same lattice; a gate on a different
				// sample point would grade a picture nobody renders. Its numbers
				// are frame-dependent under a moving lattice -- pin the phase
				// (voxel.March.HalfRes.JitterPhase) to compare single frames.
				Params->MarchView.MarchSampleJitter =
					VoxelMarchJitterForEmit(bHalfResEmit, Entry->FrameNumber);
				Params->MarchView.MarchTileCount = Entry->TileCount;
				Params->MarchVis = Entry->VisBuffer;
				Params->MarchHitT = Entry->HitDistance;
				Params->MarchSceneDepthTexture = RasterDepth;
				Params->MarchOutVerify = CountersUAV;

				const FIntPoint Size = Entry->ViewRect.Size();
				// FULL-RES DISPATCH, HALF-RES PERMUTATION, and that pairing is
				// the whole point: the gate walks every SCREEN pixel and reaches
				// the VisBuffer through the same reconstruction the emit uses, so
				// what it scores is the depth the emit would have written. A gate
				// dispatched over the half-res grid would grade the march and
				// certify nothing about the reconstruction.
				FVoxelMarchVerifyDepthCS::FPermutationDomain GatePermutation;
				GatePermutation.Set<FVoxelMarchHalfResDim>(bHalfResEmit);
				TShaderMapRef<FVoxelMarchVerifyDepthCS> Shader(ShaderMap, GatePermutation);
				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("VoxelMarch.VerifyDepth"), Shader, Params,
					FIntVector(FMath::DivideAndRoundUp(Size.X, FVoxelMarchVerifyDepthCS::kGroupSize),
					           FMath::DivideAndRoundUp(Size.Y, FVoxelMarchVerifyDepthCS::kGroupSize),
					           1));

				if (!Free->Readback.IsValid())
				{
					Free->Readback =
						MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelMarch.VerifyReadback"));
				}
				AddEnqueueCopyPass(GraphBuilder, Free->Readback.Get(), Counters,
				                   uint32(FVoxelMarchState::kVerifyWords) * sizeof(uint32));
				Free->Generation = ++State->VerifyGeneration;
				Free->bInFlight = true;
			}
		}
	}

	FScopeLock Guard(&State->Lock);
	State->Stats.EmitFrames++;
	if (bScratch)
	{
		State->Stats.ScratchFrames++;
	}
	// Carried with the data, not asked of the cvar at print time: the probe
	// changes what EmitGpuMs and the frame time mean.
	State->Stats.bHTileProbe = bHTileProbe;
}
