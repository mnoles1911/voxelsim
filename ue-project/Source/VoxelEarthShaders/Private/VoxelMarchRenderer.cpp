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
		TEXT("a missing level-0 chunk renders as level-1 detail instead of a hole. 0 (default) = ")
		TEXT("off, and off is the byte-identical control -- it is a SHADER PERMUTATION for the ")
		TEXT("same reason rings are. 1 is the intended arm (worst case two walks per segment); 2 ")
		TEXT("is the measurable step beyond it. Clamped to 0..2.\n")
		TEXT("THE GATE IS RESIDENCY, NOT VALIDITY: a resident-but-empty chunk is real air and ")
		TEXT("never falls through -- unconditional fallthrough would plug cave mouths, doorways ")
		TEXT("and overhangs with coarse rock, a visible regression no counter catches. Sky rays ")
		TEXT("are NOT protected by the gate (sky-band-trimmed chunks are legitimately ")
		TEXT("non-resident), so each depth step is paid on them; that is the cost ceiling.\n")
		TEXT("USELESS WITHOUT -VoxelHierarchicalCoverage on the game side: the coarse levels ")
		TEXT("must actually COVER the ground inside them for a coarser retry to find anything. ")
		TEXT("With coverage off this fires and misses, burning the extra walks for nothing. ")
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
		TEXT("at levels 1-5 (streaming never admits them); -VoxelHierarchicalCoverage must ")
		TEXT("inflate EVICTED (32,923 pool evictions measured); hovering must drain PENDING to ")
		TEXT("~0 alongside the pending queues. A large UNATTRIBUTED bucket, or a printed ")
		TEXT("attributed-vs-uncovered shortfall, indicts the instrument itself and is printed ")
		TEXT("rather than folded away. Level 1 leaves the breakdown words unwritten and the log ")
		TEXT("says NOT MEASURED for them -- never zero. INCOMPATIBLE with ")
		TEXT("voxel.March.IndexGpuResident 1: the GPU publish kernel clears cells to literal 0, ")
		TEXT("so the annotations are not written there and the whole breakdown reads ")
		TEXT("never-admitted; the writer disarms itself and the perf line says so."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarchRingCount(
		TEXT("voxel.March.RingCount"), 6,
		TEXT("How many rings the cascade walks, 1..6. 6 is the full 4 km cascade; 2 is the ")
		TEXT("B-2b-1 configuration that proved the mechanism (0-128 / 128-256 m). Clamped to the ")
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
		TEXT("voxel.March.RingOuterM"), 128.0f,
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
		TEXT("it on (it is refused there), and read the frame time, not EmitGpuMs."),
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
		            "declined noView=%llu noVolume=%llu noTextures=%llu unsupported=%llu "
		            "noPool=%llu"),
		       Arm.Mode == 0 ? TEXT("off") : (Arm.Mode == 1 ? TEXT("scene") : TEXT("scratch")),
		       Arm.Source == 0 ? TEXT("occupancy") : TEXT("brickpool"),
		       Arm.StepBudget, Arm.bAO ? 1 : 0, Arm.bDBuffer ? 1 : 0, Arm.bVelocity ? 1 : 0,
		       S.bHTileProbe ? 1 : 0,
		       *Ms(S.MarchGpuMs, S.Frames), *Ms(S.EmitGpuMs, S.EmitFrames),
		       *Ms(S.ScratchGpuMs, S.ScratchFrames), S.Frames,
		       S.EmitFrames, S.TilesTotal, S.TilesHit, S.IndexEntries,
		       S.DeclinedNoView, S.DeclinedNoVolume, S.DeclinedNoTextures,
		       S.DeclinedUnsupported, S.DeclinedNoPool);
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
					            "L5=%d | dropped above the %u indexed levels=%d (SHOULD BE 0 "
					            "at six levels; anything else is the pool building a level "
					            "the marcher cannot walk)"),
					       Idx.GetOfferedAtLevel(0), Idx.GetOfferedAtLevel(1),
					       Idx.GetOfferedAtLevel(2), Idx.GetOfferedAtLevel(3),
					       Idx.GetOfferedAtLevel(4), Idx.GetOfferedAtLevel(5),
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
	if (!GMarchState.IsValid())
	{
		return Out;
	}
	FScopeLock Guard(&GMarchState->Lock);
	const bool bArmed = Out.bArmed;
	const bool bBreakdownArmed = Out.bBreakdownArmed;
	Out = GMarchState->HoleWindow;
	Out.bArmed = bArmed;
	Out.bBreakdownArmed = bBreakdownArmed;
	// Kept for the HUD's 1 Hz peek -- the panel must show what the log drained
	// without becoming a second drainer (two drainers of one accumulator each
	// see a random share).
	GMarchState->LastDrainedHoleWindow = Out;
	GMarchState->HoleWindow = FVoxelMarchHoleStats();
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
	Out = GMarchState->LastDrainedHoleWindow;
	// The arm flags track the switch NOW, not the switch as it stood when the
	// stale window was drained -- the panel's off/armed wording follows the
	// cvar the owner just typed.
	Out.bArmed = bArmed;
	Out.bBreakdownArmed = bBreakdownArmed;
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
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
	SHADER_PARAMETER(uint32, MarchIndexCellsPerLevel)
	// PHASE 6. Bound on every permutation, including the ones compiled with
	// VOXEL_MARCH_COVER 0: a shader-side global with no entry in the struct is
	// an unbound parameter, and reach 0 is the honest "cover is off" value.
	SHADER_PARAMETER(uint32, MarchCoverIndexGrid)
	SHADER_PARAMETER(float, MarchCoverReachUU)
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
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
	SHADER_PARAMETER(uint32, MarchIndexCellsPerLevel)
	// PHASE 6. Bound on every permutation, including the ones compiled with
	// VOXEL_MARCH_COVER 0: a shader-side global with no entry in the struct is
	// an unbound parameter, and reach 0 is the honest "cover is off" value.
	SHADER_PARAMETER(uint32, MarchCoverIndexGrid)
	SHADER_PARAMETER(float, MarchCoverReachUU)
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
// Source, SkipLevels, Rings, Cover, CoverSkip, Fallthrough.
constexpr int32 kVoxelMarchWalkShapeDims = 6;

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
		                         FVoxelMarchFallthroughDim, FVoxelMarchHoleStatsDim>;

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
		// The hole counters are ring-walk properties (substituted compares the
		// walked level against the owning segment; uncovered needs per-level
		// residency), so hole stats without rings would build, bind and mean
		// nothing -- refused for the reason the pair above is. Rings already
		// imply source 1, so that pairing needs no third clause.
		if (P.Get<FVoxelMarchHoleStatsDim>() && !P.Get<FVoxelMarchRingsDim>())
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
		// The level-2 breakdown's two word groups: 6 level words then 4
		// reason words. The shader adds the ring level / the
		// VOXEL_MARCH_MISS_* code to these bases, so the group sizes are
		// layout shared with the enum -- static_asserted right below the
		// class rather than trusted.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_UNC_LEVEL0"),
		                         int32(VoxelMarchHoleWord::UncLevelFirst));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_UNC_REASON0"),
		                         int32(VoxelMarchHoleWord::UncReasonFirst));
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_HOLE_WORDS"),
		                         int32(VoxelMarchHoleWord::Count));
	}
};
// The breakdown's group sizes ARE the buffer layout (the shader indexes
// base + level and base + reason code), so the enum must agree with itself
// before anything is allowed to read a word by arithmetic.
static_assert(int32(VoxelMarchHoleWord::UncReasonFirst) -
                      int32(VoxelMarchHoleWord::UncLevelFirst) ==
                  VoxelMarchHoleWord::kNumLevels,
              "hole-stats level group size drifted from the enum layout");
static_assert(int32(VoxelMarchHoleWord::Count) -
                      int32(VoxelMarchHoleWord::UncReasonFirst) ==
                  VoxelMarchHoleWord::kNumReasons,
              "hole-stats reason group size drifted from the enum layout");
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
		                         FVoxelMarchVelocityDim, FVoxelMarchRingsDim>;

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
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
	SHADER_PARAMETER(uint32, MarchIndexCellsPerLevel)
	// PHASE 6. Bound on every permutation, including the ones compiled with
	// VOXEL_MARCH_COVER 0: a shader-side global with no entry in the struct is
	// an unbound parameter, and reach 0 is the honest "cover is off" value.
	SHADER_PARAMETER(uint32, MarchCoverIndexGrid)
	SHADER_PARAMETER(float, MarchCoverReachUU)
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
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
	SHADER_PARAMETER(uint32, MarchIndexCellsPerLevel)
	// PHASE 6. Bound on every permutation, including the ones compiled with
	// VOXEL_MARCH_COVER 0: a shader-side global with no entry in the struct is
	// an unbound parameter, and reach 0 is the honest "cover is off" value.
	SHADER_PARAMETER(uint32, MarchCoverIndexGrid)
	SHADER_PARAMETER(float, MarchCoverReachUU)
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

// Six rings to 4 km. Mirrors VOXEL_MARCH_MAX_RINGS in VoxelBrickTraverse.ush and
// FVoxelMarchChunkIndex::kLevels; all three must agree or the marcher walks a
// level the index does not carry.
static constexpr int32 kVoxelMarchMaxRings = 6;

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
	S.Count = FMath::Clamp(CVarVoxelMarchRingCount.GetValueOnAnyThread(), 1, kVoxelMarchMaxRings);
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
	FRDGBufferRef IndexBuffer = Index.Register(GraphBuilder);
	if (IndexBuffer != nullptr)
	{
		Params.MarchChunkIndex = GraphBuilder.CreateSRV(IndexBuffer, PF_R32_UINT);
	}
	Params.MarchIndexDimChunks = FUintVector(FVoxelMarchChunkIndex::kDimXY,
	                                         FVoxelMarchChunkIndex::kDimXY,
	                                         FVoxelMarchChunkIndex::kDimZ);
	Params.MarchIndexCellsPerLevel = FVoxelMarchChunkIndex::kCellsPerLevel;
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
	Poll(State->MarchTiming, MarchMs);
	Poll(State->EmitTiming, EmitMs);
	Poll(State->ScratchTiming, ScratchMs);
	if (MarchMs >= 0.0f || EmitMs >= 0.0f || ScratchMs >= 0.0f)
	{
		FScopeLock Guard(&State->Lock);
		if (MarchMs >= 0.0f) { State->Stats.MarchGpuMs = MarchMs; }
		if (EmitMs >= 0.0f) { State->Stats.EmitGpuMs = EmitMs; }
		if (ScratchMs >= 0.0f) { State->Stats.ScratchGpuMs = ScratchMs; }
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
			Slot.Readback->Lock(uint32(VoxelMarchHoleWord::Count) * sizeof(uint32)));
		if (Src != nullptr)
		{
			const uint32 Rays = Src[VoxelMarchHoleWord::Rays];
			const uint32 Hits = Src[VoxelMarchHoleWord::Hits];
			const uint32 Substituted = Src[VoxelMarchHoleWord::Substituted];
			const uint32 Uncovered = Src[VoxelMarchHoleWord::Uncovered];
			// The breakdown words, copied out BEFORE Unlock invalidates Src.
			// Read regardless of level (the buffer always has Count words),
			// folded in only for level-2 frames below.
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
// HOOK 1 -- stash
// ---------------------------------------------------------------------------
void FVoxelMarchRenderExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder,
                                                            FSceneView& InView)
{
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
	Entry.InvDeviceZToWorldZ = InView.InvDeviceZToWorldZTransform;
	Entry.ViewUniformBuffer = InView.ViewUniformBuffer.GetReference();

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
		const FIntPoint TileCount(FMath::DivideAndRoundUp(Size.X, kVoxelMarchTileSize),
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
		// Sized to the VIEW RECT and indexed in view-local pixels, not to the
		// scene extent. A split-screen or stereo family gets one of these per
		// view and they cannot alias.
		FRDGTextureRef Vis = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(Size, PF_R32G32_UINT, FClearValueBinding::None,
			                          TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("VoxelMarch.Vis"));
		FRDGTextureRef HitT = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(Size, PF_R32_FLOAT, FClearValueBinding::None,
			                          TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("VoxelMarch.HitT"));

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
		MarchView.MarchInvDeviceZToWorldZ = Entry.InvDeviceZToWorldZ;
		MarchView.MarchPixelConeSlope = Entry.PixelConeSlope;
		MarchView.MarchClimateStrength = CVarVoxelMarchClimateStrength.GetValueOnRenderThread();
		MarchView.MarchTileCount = TileCount;

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
					                                 int32(VoxelMarchHoleWord::Count)),
					TEXT("VoxelMarch.HoleStats"));
				FRDGBufferUAVRef HoleStatsUAV =
					GraphBuilder.CreateUAV(HoleStats, PF_R32_UINT);
				AddClearUAVPass(GraphBuilder, HoleStatsUAV, 0u);
				Params->MarchOutHoleStats = HoleStatsUAV;
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
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("VoxelMarch.March(%dx%d, budget %d)", Size.X, Size.Y,
				                             Arm.StepBudget),
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
					                   uint32(VoxelMarchHoleWord::Count) * sizeof(uint32));
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

			TShaderMapRef<FVoxelMarchDepthOnlyVS> VertexShader(ShaderMap);
			TShaderMapRef<FVoxelMarchDepthOnlyPS> PixelShader(ShaderMap);
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
				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("VoxelMarch.VerifySource"),
					ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, CShader, CParams,
					FIntVector(TileCount.X, TileCount.Y, 1));

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
		Params->MarchView.MarchInvDeviceZToWorldZ = Entry->InvDeviceZToWorldZ;
		Params->MarchView.MarchPixelConeSlope = Entry->PixelConeSlope;
		Params->MarchView.MarchClimateStrength = CVarVoxelMarchClimateStrength.GetValueOnRenderThread();
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

		TShaderMapRef<FVoxelMarchEmitVS> VertexShader(ShaderMap);
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
				Params->MarchView.MarchInvDeviceZToWorldZ = Entry->InvDeviceZToWorldZ;
				Params->MarchView.MarchPixelConeSlope = Entry->PixelConeSlope;
		Params->MarchView.MarchClimateStrength = CVarVoxelMarchClimateStrength.GetValueOnRenderThread();
				Params->MarchView.MarchTileCount = Entry->TileCount;
				Params->MarchVis = Entry->VisBuffer;
				Params->MarchHitT = Entry->HitDistance;
				Params->MarchSceneDepthTexture = RasterDepth;
				Params->MarchOutVerify = CountersUAV;

				const FIntPoint Size = Entry->ViewRect.Size();
				TShaderMapRef<FVoxelMarchVerifyDepthCS> Shader(ShaderMap);
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
