#include "VoxelGI.h"

#include "VoxelChunkComponent.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelLightField.h"
// VoxelEarth -> VoxelEarthShaders, never the reverse (see VoxelEarth.Build.cs).
// So the encoder and its driver live HERE, in the module that owns the light
// field, and call into the VOXELEARTHSHADERS_API volume rather than having the
// renderer module reach back for the field.
#include "VoxelGIVolume.h"
// P7: the GPU cone march over the resident bricks. VoxelEarth ->
// VoxelEarthShaders, the same direction every other include here runs.
#include "VoxelGIMarchPass.h"
#include "VoxelGpuPoolComponent.h"
// voxel.GI.VolumeDigTest carves through the real edit path (CarveSphere) rather
// than poking the field, so the dig test exercises the same remesh -> ingest ->
// upload chain a player's dig does.
#include "VoxelWorldSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/PointLightComponent.h"
#include "Engine/Engine.h"
// voxel.GI.LocalLightTest spawns a REAL deferred point light beside the un-crush
// it registers. The un-crush is a BaseColor change and emits nothing, so without
// a light to un-crush for there is nothing to photograph.
#include "Engine/PointLight.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGI, Log, All);

// --- cvars -----------------------------------------------------------------
//
// Naming follows this module's existing convention (voxel.<Area>.<Name>, see
// VoxelDebug.cpp). Declared here rather than in VoxelDebug.cpp purely as file
// ownership hygiene while several agents are live in this module.

namespace
{
	// DEFAULT IS 1, AND THE HELP TEXT USED TO SAY 0.
	//
	// It read "0 = off (default, and genuinely zero-cost...)" while the line
	// above it had said 1 since 2026-07-27. A cvar whose description contradicts
	// its own default one line up is worse than an undocumented one: it is
	// believed. It was, downstream -- docs/gpu-roadmap-remaining.md:101 still
	// asserts this and voxel.GI.Volume "remain 0 on main", and a Phase 7 brief
	// was written on the premise that GI ships off. Every leg on this project,
	// including the Arm A ceiling in
	// docs/measurements/armA-drawpath-ceiling-2026-08-19.txt, has run with GI
	// ON, so GI's cost is already inside that 18.99 / 21.52 ms baseline rather
	// than waiting outside it.
	//
	// The "genuinely zero-cost when 0" claim IS still true and is kept, because
	// it is the property that makes -VoxelGIOff a valid control arm: with this
	// at 0 the subsystem clears its state and returns from Tick, the scene proxy
	// emits byte-identical vertex colours, the volume is not sampled, and
	// WantsChunkQuads stops forcing chunks off the direct-to-pool path.
	TAutoConsoleVariable<int32> CVarGIEnabled(
		TEXT("voxel.GI.Enabled"), 1,   // 2026-07-27: ON for the manual PIE evaluation.
		TEXT("Voxel light field + cone-traced GI (M4). DEFAULT 1 (on) since 2026-07-27. ")
		TEXT("0 = off, and genuinely zero-cost: the subsystem does not tick, the scene proxy emits ")
		TEXT("byte-identical vertex colours, and WantsChunkQuads stops forcing near-field chunks off ")
		TEXT("the direct-to-pool path. Use -VoxelGIOff to set it from the COMMAND LINE -- an ")
		TEXT("-ExecCmds toggle lands after streaming has begun and measures a mixed state. ")
		TEXT("CLIENT-SIDE RENDERING ONLY -- outside the determinism boundary."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarGIStrength(
		TEXT("voxel.GI.Strength"), 1.0f,
		TEXT("Blend between the mesher's geometric AO alone (0) and the full cone-traced term (1)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIAmbientFloor(
		TEXT("voxel.GI.AmbientFloor"), 0.06f,
		TEXT("Minimum ambient a fully enclosed surface keeps. Pure zero reads as a rendering bug ")
		TEXT("rather than as darkness, and there is no other light source in a sealed tunnel."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxQuadSpanVoxels(
		TEXT("voxel.GI.MaxQuadSpanVoxels"), 8,
		TEXT("When GI is on, greedy quads are subdivided so no quad spans more than this many ")
		TEXT("voxels. GI is evaluated per VERTEX, so an unsubdivided 32-voxel quad would smear a ")
		TEXT("3.2m gradient across 4 corner samples. 0 disables subdivision."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIRadiusUU(
		TEXT("voxel.GI.RadiusUU"), 7000.f,
		TEXT("Light field build radius around the view origin, UU. Deliberately larger than the R0 ")
		TEXT("ring (6400 UU) so every level-0 chunk has field coverage and there is no partial-")
		TEXT("coverage seam inside R0."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIFadeStartUU(
		TEXT("voxel.GI.FadeStartUU"), 4800.f,
		TEXT("Distance from the field centre at which GI starts blending back to plain geometric AO."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIFadeEndUU(
		TEXT("voxel.GI.FadeEndUU"), 6400.f,
		TEXT("Distance at which GI is fully faded out (matches the R0 ring outer radius)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxVoxelizePerFrame(
		TEXT("voxel.GI.MaxVoxelizePerFrame"), 16,
		TEXT("Chunks converted into light field bricks per frame. Bounds the streaming ramp."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxBrickSolvesPerFrame(
		TEXT("voxel.GI.MaxBrickSolvesPerFrame"), 8,
		TEXT("Bricks cone-traced per frame (blocking ParallelFor over workers). THIS is the knob ")
		TEXT("that keeps a large explosion from stalling the frame: a big edit makes the dirty ")
		TEXT("queue longer, never the frame."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIRefreshBricksPerFrame(
		TEXT("voxel.GI.RefreshBricksPerFrame"), 2,
		TEXT("Resident bricks re-solved per frame in round-robin, on top of the dirty queue. This ")
		TEXT("is what lets the progressive one-bounce gather converge."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxChunkRefreshesPerFrame(
		TEXT("voxel.GI.MaxChunkRefreshesPerFrame"), 4,
		TEXT("Chunks re-shaded per frame to pick up new GI vertex colours. Same order of ")
		TEXT("magnitude as the streaming system's own applies-per-frame budget. (Before the ")
		TEXT("in-place colour update this was a SCENE PROXY REBUILD per chunk -- see ")
		TEXT("voxel.GI.LegacyProxyRebuild.)"),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGILegacyProxyRebuild(
		TEXT("voxel.GI.LegacyProxyRebuild"), 0,
		TEXT("1 = restore the pre-2026-07-22 re-shade path: push new GI vertex colours by calling ")
		TEXT("MarkRenderStateDirty(), i.e. destroy and rebuild the whole scene proxy, instead of ")
		TEXT("memcpying the colours into the existing colour vertex buffer. Kept solely so the ")
		TEXT("before/after cost can be measured from ONE binary in an interleaved A/B ")
		TEXT("(-VoxelGILegacyRefresh); it is strictly slower for identical output."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIEditDirtyRadiusBricks(
		TEXT("voxel.GI.EditDirtyRadiusBricks"), 2,
		TEXT("Bricks around an edited brick that are re-solved. 2 -> a 5x5x5 neighbourhood (1.6m ")
		TEXT("of light bleed around the change)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxBricks(
		TEXT("voxel.GI.MaxBricks"), 4096,
		TEXT("Hard cap on resident light field bricks (~3.6 KB each, so 4096 ~= 15 MB)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIConeDistanceUU(
		TEXT("voxel.GI.ConeDistanceUU"), 3000.f,
		TEXT("Cone march termination distance, UU."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIBounceAlbedo(
		TEXT("voxel.GI.BounceAlbedo"), 0.4f,
		TEXT("Diffuse albedo used for the progressive one-bounce gather."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGILegacyConeBasis(
		TEXT("voxel.GI.LegacyConeBasis"), 0,
		TEXT("1 = restore the pre-2026-07-21 DEGENERATE cone basis: 6 axis-aligned cones, each ")
		TEXT("ambient-cube slot fed by its own axis alone. Because every normal this mesh produces ")
		TEXT("is axis-aligned, max(0,N.D) then selects exactly one cone and side light is weighted ")
		TEXT("to zero. Kept solely so the before/after of the fix can be captured from one build ")
		TEXT("(-VoxelGILegacyBasis); it is a bug, not a quality/perf tradeoff."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIDebug(
		TEXT("voxel.GI.Debug"), 0,
		TEXT("1 = log light field stats (bricks, queue depths, solve ms) once a second. ")
		TEXT("2 = + a downward-normal probe column and a per-chunk shading summary. ")
		TEXT("3 = + a PER-FACE-DIRECTION breakdown of every chunk proxy (which was what ")
		TEXT("finally isolated the roof-underside defect: the +X/-X/+Y/-Y/+Z/-Z buckets ")
		TEXT("report their own hit rate and mean shade, so 'this normal never finds data' ")
		TEXT("is one log line rather than a guess from a screenshot)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIDebugVis(
		TEXT("voxel.GI.DebugVis"), 0,
		TEXT("Diagnostic override of the vertex shade byte (read at proxy build, so use ")
		TEXT("-VoxelGIVis=<n> rather than -ExecCmds). 0 = off. 1 = raw sampled irradiance, ")
		TEXT("with a MISS forced to white -- distinguishes 'the field says lit' from 'the ")
		TEXT("field had no data and we fell back to plain AO'. 2 = pure hit/miss map ")
		TEXT("(hit = black, miss = white). 3 = |N.Z| ramp, to confirm which faces are ")
		TEXT("actually on screen. 4 = omit +Z faces. 5 = omit -Z faces. 6 = restore the ")
		TEXT("LEGACY inverted triangle winding (the roof-underside defect), for before/after ")
		TEXT("capture from a single build."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxBrickUploadsPerFrame(
		TEXT("voxel.GI.MaxBrickUploadsPerFrame"), 64,
		TEXT("Light field bricks whose texels are pushed into the GPU GI volume per frame. This is ")
		TEXT("the pooled path's replacement for voxel.GI.MaxChunkRefreshesPerFrame -- a pooled chunk ")
		TEXT("has no component and no vertex colours to re-shade, so lighting updates are texel ")
		TEXT("uploads instead. Bricks are merged into X-runs before uploading, so this bounds ")
		TEXT("BYTES, not draw-call count."),
		ECVF_Default);

	// docs/sky-and-local-light-plan.md §2.2 / L1. MIRRORS
	// voxel.GI.MaxBrickUploadsPerFrame above, in the same units the local splat
	// actually works in: dirty BOXES, not bricks. Every box is at most 8 local
	// texels deep in Z -- a light's neighbourhood is SLICED to that depth by
	// MarkLocalLightDirty precisely so that this number bounds something, since one
	// box per light would put a whole light's marches in one frame no matter what
	// this is set to.
	TAutoConsoleVariable<int32> CVarGIMaxLocalUploadsPerFrame(
		TEXT("voxel.GI.MaxLocalUploadsPerFrame"), 16,
		TEXT("Local-light splat boxes recomputed and uploaded into VolumeLocal per frame. The local ")
		TEXT("counterpart of voxel.GI.MaxBrickUploadsPerFrame. A box is at most 8 local texels deep: at the ")
		TEXT("default LocalVolumeDim 96 (80 UU texels) a 10 m REACH torch is 27^3 texels = 79 KB, sliced into ")
		TEXT("4 boxes of ~5k texels, and the work inside each is the inverse-square falloff plus one ")
		TEXT("occlusion march (<= 25 opacity taps, early-out on the first hit) per texel. NOTE the reach, not ")
		TEXT("the diameter: the plan's '~12^3 texels, ~7 KB' figure is for a 160 UU texel and is 11x low ")
		TEXT("here. This is what keeps 'ten torches were just placed' a longer queue rather than a longer ")
		TEXT("frame."),
		ECVF_Default);

	// The falsification knob for the half of L1 that is actually research (the
	// plan's own §7 item 4: splat occlusion quality at 80 UU). With this at 0 the
	// splat is pure inverse-square and a torch lights through walls, which is what
	// makes "the march is doing something" a measurement rather than a claim.
	TAutoConsoleVariable<int32> CVarGILocalLightOcclusion(
		TEXT("voxel.GI.LocalLightOcclusion"), 1,
		TEXT("1 = a local light's un-crush is attenuated by a line-march through the light field's level-0 ")
		TEXT("opacity, so a wall occludes. 0 = pure inverse-square with no occlusion at all -- kept so the ")
		TEXT("march's contribution can be captured as an A/B from ONE build, exactly as ")
		TEXT("voxel.GI.LegacyConeBasis is. 0 is a diagnostic, not a performance setting."),
		ECVF_Default);

	// Whether voxel.GI.LocalLightTest also spawns the real deferred light.
	//
	// THIS IS THE INSTRUMENT FOR §5 ITEM 9 (double-counting a hero light). With it
	// at 0 the un-crush is placed with NO light to un-crush for, and the wall must
	// stay dark: the un-crush is a BaseColor change, not a light, and a build where
	// it brightens a wall on its own is adding energy from nowhere.
	TAutoConsoleVariable<int32> CVarGILocalLightDeferred(
		TEXT("voxel.GI.LocalLightDeferred"), 1,
		TEXT("1 = voxel.GI.LocalLightTest also spawns the UPointLightComponent that actually emits (the ")
		TEXT("hero light; the un-crush only gives it a BaseColor to work against). 0 = register the ")
		TEXT("un-crush alone, which is the control arm: with no deferred light the wall MUST stay dark, ")
		TEXT("because A is a relative modulation and not a light source."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIVolumeCheck(
		TEXT("voxel.GI.VolumeCheck"), 0,
		TEXT("Numeric field-vs-volume equivalence harness. Non-zero arms it; the value is the number ")
		TEXT("of random solved cells to sample (1 = the default 4096). Runs ONCE, after the field has ")
		TEXT("settled, and logs VOLUMECHECK: cells=.. meanAbsErr=.. maxAbsErr=.. in irradiance bytes. ")
		TEXT("Set back to 0 to re-arm."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIVolumeCheckSettle(
		TEXT("voxel.GI.VolumeCheckSettleSeconds"), 20.f,
		TEXT("Seconds after the first texel upload before voxel.GI.VolumeCheck runs. Measuring a ")
		TEXT("field that has barely streamed in reports a tiny sample count and proves nothing."),
		ECVF_Default);

	// P7-c ARM: RETIRE THE CPU QUAD INGEST, WITH THE OLD PATH AS A LIVE CONTROL.
	//
	// WHAT THIS IS AND, MORE IMPORTANTLY, WHAT IT IS NOT.
	//
	// Feeding the light field from the resident brick volume is the whole
	// performance case for Phase 7, and that case is a LOADING case, not a frame
	// case (docs/ray-marching-plan-2026-08-19.md §13: "What survives, and it is
	// not frame time"). With GI on, every level-0 chunk within
	// voxel.GI.RadiusUU * 1.25 = 87.5 m is forced OFF the D1 direct-to-pool path
	// and onto the GPU readback path purely so this subsystem can have its
	// quads -- 806 of 1,319 level-0 candidates (61%) at fill, and exactly zero
	// at settle, which is why every settled-pose GI measurement ever taken
	// missed it.
	//
	// AT 1 THIS ARM REMOVES THE DEMAND AND NOTHING ELSE. WantsChunkQuads stops
	// retaining, both ingest hooks stop accepting, and the light field is
	// therefore FED BY NOTHING. That is deliberate and it is the point: it
	// BOUNDS THE PRIZE before the brick-sourced march is built, exactly as Arm A
	// bounded the draw-path prize before the marcher existed. What it measures
	// is cold fill and chunks/s against the 35.4 s control, with
	// quadsRetainedForGI at 0 -- and both halves have to move together.
	//
	// IT IS NOT A LIGHTING ARM AND MUST NEVER BE READ AS ONE. A field with no
	// input solves nothing, every cell reads back invalid, and the vertex
	// factory falls through to plain geometric AO. A capture from this arm looks
	// like GI being off, because for lighting purposes it is. The GI stats line
	// says so on every window, and tools\voxel-leg-summary.ps1 REFUSES to
	// summarise a leg that ran with this on unless -AllowInvalid is passed --
	// because a caveat in a column gets dropped when the number is copied into a
	// doc, and a missing row cannot be.
	//
	// WHY THE MARCH ITSELF IS NOT RE-POINTED IN THIS CHANGE: the resident brick
	// volume has NO CPU-SIDE OCCUPANCY on the shipping path. The pool is fed by
	// FVoxelBrickPool::AddChunkFromGpu from a GPU payload; the CPU keeps only
	// keys and slots (FVoxelBrickIndexEntry), and there is no brick readback
	// anywhere. So a CPU cone march cannot read the bricks at all, and the
	// re-point necessarily moves the march onto the GPU. See the report
	// accompanying this change; that is a separate, larger build, and shipping
	// half of it would leave the tree in the one state that is neither the old
	// version nor the new one.
	TAutoConsoleVariable<int32> CVarGISourceBricks(
		TEXT("voxel.GI.SourceBricks"), 0,
		TEXT("P7-c MEASUREMENT ARM. 0 (default) = the shipping CPU quad ingest, the live control. ")
		TEXT("1 = the quad ingest is RETIRED: WantsChunkQuads stops pulling near-field level-0 chunks ")
		TEXT("off the D1 direct-to-pool path and both ingest hooks stop accepting. THE LIGHT FIELD IS ")
		TEXT("THEN FED BY NOTHING -- this arm measures the STREAMING refund (cold fill, chunks/s, ")
		TEXT("quadsRetainedForGI -> 0), NOT lighting. Captures from it look like GI off because for ")
		TEXT("lighting purposes it is. Set it on the COMMAND LINE: an -ExecCmds toggle lands after ")
		TEXT("streaming has begun and measures a mixed state."),
		ECVF_Default);

	// P7 MARCH ARM: THE CONE MARCH AS A SHADER READ OF THE RESIDENT BRICKS.
	//
	// voxel.GI.SourceBricks above removes the quad DEMAND and leaves the field
	// fed by nothing -- it bounds the streaming prize and is not a lighting arm.
	// THIS one removes the demand AND replaces the producer: the dirty-brick
	// keys still arrive (a pooled apply notifies with an empty quad array on the
	// direct-to-pool path, so the key is free), and the irradiance is produced
	// by VoxelGIMarch.usf reading bricks that are already resident.
	//
	// TWO THINGS ABOUT THIS ARM LOOK EXACTLY LIKE BUGS AND ARE NOT:
	//
	//   * ENCLOSED SPACES ARE DARKER. v1 has no bounce. The CPU field is a
	//     Jacobi iteration whose cone picks up the previously-solved irradiance
	//     of whatever it lands on; doing that on the GPU means reading the two
	//     volumes while writing them, which inside one dispatch is undefined
	//     rather than stale, and doing it honestly costs a ping-pong pair
	//     (+56.6 MB at the shipping dim). So a cave is lit by sky visibility
	//     alone.
	//   * CONES ARE BINARY, not aperture-integrated. Classic VCT accumulates
	//     partial occlusion from successively coarser mips; there is no
	//     near-field pyramid to accumulate from (see the .usf header), so each
	//     cone is a visibility ray.
	//
	// Both are real differences from the control arm and BOTH ARE FOR THE OWNER
	// TO JUDGE ON SCREENSHOTS. They are stated in the arm log line for the same
	// reason they are stated here: "darker in caves" is also what a broken
	// march looks like, and the two must not be confusable.
	TAutoConsoleVariable<int32> CVarGIMarchBricks(
		TEXT("voxel.GI.MarchBricks"), 0,
		TEXT("P7. 0 (default) = the shipping CPU quad ingest + CPU cone solve, the live control arm. ")
		TEXT("1 = the CPU voxelize/solve/encode/upload chain is RETIRED and irradiance is produced by ")
		TEXT("a compute pass that cone-marches the resident brick pool directly into the GI volume. ")
		TEXT("NEEDS voxel.GI.VolumeUAV=1 AT STARTUP -- without it the volumes are not UAV-capable and ")
		TEXT("the pass refuses (logged, once). v1 has NO BOUNCE and BINARY cones, so caves read darker ")
		TEXT("than the control arm; that is the arm, not a defect. Set on the COMMAND LINE."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMarchWalk(
		TEXT("voxel.GI.MarchWalk"), 0,
		TEXT("Which brick walk the GI cone march uses. 0 (default) = FLAT (VoxelMarchTraverseBrick) -- ")
		TEXT("correct by construction and cannot inherit a skip defect. 1 = HIERARCHICAL (brick mask + ")
		TEXT("chunk mask), cheaper, and carries whatever the skip walk carries. 2 = BOTH, counting ")
		TEXT("where they disagree PER CONE DIRECTION. The default is flat DELIBERATELY: the ")
		TEXT("hierarchical walk has an open, unexplained, non-directional burst phenomenon against a ")
		TEXT("frozen world hash, and in a cone march a walk that intermittently misses content reads ")
		TEXT("as light leaking through a wall."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMarchBricksPerFrame(
		TEXT("voxel.GI.MarchBricksPerFrame"), 8,
		TEXT("GI bricks dispatched to the GPU cone march per frame. The march arm's equivalent of ")
		TEXT("voxel.GI.MaxBrickSolvesPerFrame, and it does the same job: a large edit makes the queue ")
		TEXT("longer, never the frame."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMarchStats(
		TEXT("voxel.GI.MarchStats"), 0,
		TEXT("1 = the GI march shader accumulates its per-dispatch counters. THERE IS NO READBACK ")
		TEXT("PATH: nothing reads this buffer, so at 1 all this does is pay for the atomics. It ")
		TEXT("exists so the walk-comparison permutation has somewhere to write when a readback is ")
		TEXT("built; leave it at 0."),
		ECVF_Default);

	// P7-a. THE MUTATION THAT PROVES THE IDENTITY GATE CAN FAIL.
	//
	// A gate that cannot come out the other way carries no information, and
	// this project has shipped several that could not. The pool-identity check
	// is exactly that shape: in a healthy tree it passes on every leg forever,
	// so "it passed" is indistinguishable from "it never ran". This cvar breaks
	// the NAME half of the identification on purpose, so the refusal path can
	// be observed in a real session:
	//
	//   voxel.GI.PoolIdentityMutate 1   ->  EnsureVolumeOrigin must log
	//                                       "POOL IDENTITY REFUSED" at Error,
	//                                       the volume must NOT anchor, and
	//                                       "VoxelGI volume: ANCHORED" must be
	//                                       ABSENT from the log.
	//   voxel.GI.PoolIdentityMutate 0   ->  "ANCHORED to the TERRAIN pool"
	//
	// Set it on the COMMAND LINE (-ExecCmds lands after the origin has already
	// latched, and the latch is once per session). It mutates the CHECK, never
	// the pool, so a mutated run cannot corrupt anything: it can only refuse.
	TAutoConsoleVariable<int32> CVarGIPoolIdentityMutate(
		TEXT("voxel.GI.PoolIdentityMutate"), 0,
		TEXT("TEST ONLY. 1 = force the terrain-pool NAME check in EnsureVolumeOrigin to fail, so the ")
		TEXT("P7-a refusal path is observable. Expect 'POOL IDENTITY REFUSED' at Error and NO ")
		TEXT("'VoxelGI volume: ANCHORED' line. Proves the gate can fail; a gate that cannot fail is ")
		TEXT("not a gate. Never set outside that check."),
		ECVF_Default);

	constexpr int32 kMaxPendingVoxelize = 1024;
	constexpr int32 kCoarseRebuildIntervalFrames = 30;

	// Bricks of gap tolerated when coalescing an X-run. A run that skips two
	// clean bricks still beats a second UpdateTexture3D and its barrier, and a
	// dig dirties a contiguous 5x5x5 neighbourhood where the gaps are rare
	// anyway.
	constexpr int32 kVolumeRunGapBricks = 2;

	constexpr int32 kVolumeCheckDefaultSamples = 4096;

	// One brick-run staged for the render thread. Owns its bytes: the light
	// field is read on the GAME thread under FReadScope and the payload crosses
	// in the ENQUEUE_RENDER_COMMAND, never the other way round.
	struct FVoxelGIVolumeRun
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector Size = FIntVector::ZeroValue;
		TArray<uint8> TexelsPos;
		TArray<uint8> TexelsNeg;
	};

	// The pool component's world location IS pool-primitive space's origin --
	// the component carries the world's big offset in its double-precision
	// transform and everything the shader sees is relative to it
	// (docs/gpu-gi-volume-design.md §6). Read off the live component rather than
	// recomputed from the streaming subsystem's rebase so there is exactly one
	// definition of the space.
	//
	// P7-a, FIXED 2026-08-20. THIS USED TO TAKE THE FIRST POOL TObjectIterator
	// HANDED BACK. It no longer picks; it IDENTIFIES, and it REFUSES when it
	// cannot.
	//
	// THERE ARE UP TO FIVE UVoxelGpuPoolComponents IN A LIVE WORLD, and the
	// count in the previous version of this comment (four) was itself wrong:
	//   terrain               VoxelWorldSubsystem.cpp   GetOrCreateGpuPool
	//   water, ONE PER BUCKET VoxelWaterSubsystem.cpp   SpawnWaterPoolPrimitive
	//   region verify         VoxelGpuVerify.cpp        (voxel.GPU.VerifyRegion)
	//   pool-write verify     VoxelGpuMeshAsyncVerify.cpp
	//   automation test       VoxelGpuGeometryPoolTests.cpp  (never registered)
	//
	// They sit at unrelated world locations BY CONSTRUCTION -- terrain at
	// GpuPoolRebase = FirstChunkOrigin, water at WaterPoolBucketRebase(bucket),
	// and the pool-write harness at the world origin because it never calls
	// SetWorldLocation at all. The caller computes
	// OriginPoolUU = FVector3f(VolumeOriginWorldUU - PoolWorldUU), so a wrong
	// pick offsets every GI sample by the difference of two unrelated rebases.
	// The symptom is not an error: it is lighting sampled from nowhere, and
	// because VoxelQuadVertexFactory.ush gates on bInsideVolume what a player
	// sees is GI being ABSENT while everything still renders.
	//
	// WHY NOT IsWaterMode(). Because it is not identity. Terrain and BOTH
	// verify pools are non-water, and until P7-a all three also answered
	// GetPoolName() with the same default. Water is the loudest wrong answer,
	// not the only one, and a filter that removes only the loudest one would
	// look like a fix while leaving two ways to be silently wrong.
	//
	// TWO INDEPENDENT IDENTIFICATIONS, AND THEY MUST AGREE:
	//   1. OWNERSHIP -- UVoxelWorldSubsystem::GetTerrainGpuPool() returns the
	//      pool that subsystem itself created. That is not a heuristic about
	//      the terrain pool; it is the definition of it.
	//   2. NAME -- GetPoolName() == VoxelGpuPool::kTerrainPoolName, set at the
	//      construction site.
	// (1) alone is sufficient for correctness. (2) is here because (1) is a
	// join across two modules, and this project's standing failure mode is a
	// join that is COMPUTED rather than CHECKED. A disagreement means one of
	// the two ends is stale; it is logged at Error and REFUSED, never resolved
	// in favour of whichever looks more likely.
	//
	// REFUSING IS SAFE HERE, AND THE OLD COMMENT'S OBJECTION TO IT DOES NOT
	// APPLY. That objection was "water pools always exist, so a refusal would
	// disable the volume in the shipping configuration". It was an objection to
	// refusing when the CENSUS IS AMBIGUOUS. With positive identification the
	// only refusal left in a healthy tree is "the terrain subsystem has no pool
	// yet", which is exactly the pre-existing deferral: EnsureVolumeOrigin is
	// called every tick until it succeeds, and the terrain pool is created
	// lazily by the first pooled chunk. A refusal therefore costs a tick, not a
	// feature. It becomes permanent only where no terrain pool is ever created
	// (voxel.Stream.GPU 0), and there nothing samples the volume anyway.
	//
	// OutNumCandidates still reports the FULL registered census, INCLUDING the
	// pools this function now correctly ignores, so the log can say how many
	// wrong answers were on offer. It is telemetry and not a decision input --
	// nothing below branches on it.
	enum class EFindTerrainPoolOutcome : uint8
	{
		Found,
		NoWorldSubsystem, // no UVoxelWorldSubsystem in this world
		NoTerrainPoolYet, // subsystem present, pool not created yet: DEFER
		NameMismatch,     // the subsystem's own pool is not named as terrain
	};

	// Split out so the three outcomes above can be logged differently. Folding
	// them into one bool was the previous shape, and it is what let "no pool
	// yet" (benign, expected on frame 0) and "found a pool that is not ours"
	// (a wiring defect) print the same reassuring line.
	int32 CountRegisteredPools(const UWorld* World)
	{
		int32 Count = 0;
		for (TObjectIterator<UVoxelGpuPoolComponent> It; It; ++It)
		{
			const UVoxelGpuPoolComponent* Pool = *It;
			if (Pool && !Pool->IsTemplate() && Pool->IsRegistered() && Pool->GetWorld() == World)
			{
				++Count;
			}
		}
		return Count;
	}

	EFindTerrainPoolOutcome FindTerrainPoolWorldLocation(const UWorld* World, FVector& OutLocation,
	                                                     int32* OutNumCandidates = nullptr)
	{
		if (OutNumCandidates)
		{
			*OutNumCandidates = World ? CountRegisteredPools(World) : 0;
		}
		if (!World)
		{
			return EFindTerrainPoolOutcome::NoWorldSubsystem;
		}
		const UVoxelWorldSubsystem* Terrain = World->GetSubsystem<UVoxelWorldSubsystem>();
		if (!Terrain)
		{
			return EFindTerrainPoolOutcome::NoWorldSubsystem;
		}
		const UVoxelGpuPoolComponent* Pool = Terrain->GetTerrainGpuPool();
		if (!Pool || !Pool->IsRegistered())
		{
			return EFindTerrainPoolOutcome::NoTerrainPoolYet;
		}
		// Identification 2. See above: a disagreement is a stale join, and the
		// only safe response to a stale join is to stop rather than to pick.
		// CVarGIPoolIdentityMutate exists so this branch is reachable in a
		// healthy tree -- see its declaration.
		const bool bMutate = CVarGIPoolIdentityMutate.GetValueOnGameThread() != 0;
		if (bMutate || Pool->GetPoolName() != VoxelGpuPool::kTerrainPoolName)
		{
			return EFindTerrainPoolOutcome::NameMismatch;
		}
		OutLocation = Pool->GetComponentLocation();
		return EFindTerrainPoolOutcome::Found;
	}
}

namespace VoxelGI
{
	bool IsEnabled() { return CVarGIEnabled.GetValueOnAnyThread() != 0; }
	float GetStrength() { return FMath::Clamp(CVarGIStrength.GetValueOnAnyThread(), 0.f, 1.f); }
	float GetAmbientFloor() { return FMath::Clamp(CVarGIAmbientFloor.GetValueOnAnyThread(), 0.f, 1.f); }
	// -VoxelGIQuadSpan=<n> overrides voxel.GI.MaxQuadSpanVoxels from the COMMAND
	// LINE, latched on first use. This knob is read at proxy-build time, so an
	// -ExecCmds cvar only affects chunks meshed AFTER it lands -- chunks already
	// resident keep the old subdivision until something re-meshes them, which
	// silently mixes both settings into one measurement. Same class of trap as
	// the -ExecCmds voxel.GI.Enabled A/B that cost three runs.
	int32 GetMaxQuadSpanVoxels()
	{
		static const int32 CmdLineOverride = []
		{
			int32 Value = -1;
			return FParse::Value(FCommandLine::Get(), TEXT("VoxelGIQuadSpan="), Value) ? FMath::Clamp(Value, 0, 32) : -1;
		}();
		if (CmdLineOverride >= 0)
		{
			return CmdLineOverride;
		}
		return FMath::Clamp(CVarGIMaxQuadSpanVoxels.GetValueOnAnyThread(), 0, 32);
	}
	float GetFadeStartUU() { return CVarGIFadeStartUU.GetValueOnAnyThread(); }
	float GetFadeEndUU() { return CVarGIFadeEndUU.GetValueOnAnyThread(); }
	int32 GetDebugLevel() { return CVarGIDebug.GetValueOnAnyThread(); }
	int32 GetDebugVis() { return CVarGIDebugVis.GetValueOnAnyThread(); }
	// P7. ONE reader per arm, so the consumers and the stats line cannot
	// disagree about which arm a leg is on.
	//
	// THE MARCH ARM RETIRES THE QUAD INGEST TOO, and that is why this is an OR
	// rather than two independent switches. Both arms remove the same demand;
	// they differ only in what replaces it (nothing, versus the GPU march). A
	// march arm that still pulled chunks off the direct-to-pool path would be
	// paying the whole streaming cost this phase exists to delete.
	bool IsMarchBricksEnabled() { return CVarGIMarchBricks.GetValueOnAnyThread() != 0; }
	bool IsQuadIngestRetired()
	{
		return CVarGISourceBricks.GetValueOnAnyThread() != 0 || IsMarchBricksEnabled();
	}
}

// --- subsystem lifetime ----------------------------------------------------

void UVoxelGISubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Field = MakeUnique<FVoxelLightField>();

	// Latch which arm the command line asked for, so Tick can keep checking that
	// it HELD rather than trusting the one read-back GameMode did at init. See
	// the member declarations: the init read-back cannot see an -ExecCmds
	// override that lands later in startup, and SetByConsole outranks the
	// SetByCode the switch used. Parsed here rather than passed from GameMode so
	// this works whatever spawns the world.
	{
		const bool bArmOn = FParse::Param(FCommandLine::Get(), TEXT("VoxelGIOn"));
		const bool bArmOff = FParse::Param(FCommandLine::Get(), TEXT("VoxelGIOff"));
		if (bArmOn != bArmOff) // both, or neither, is not an arm -- GameMode logs the both case
		{
			ArmRequestedEnabled = bArmOn ? 1 : 0;
		}
	}

	// -VoxelGIConverge=<N> -- the energy-conservation proof harness. Read from
	// the command line at init, NOT via -ExecCmds: cvars set through ExecCmds
	// land after subsystems initialise, and the whole point of this harness is
	// to control what state the solve starts from (this module already learned
	// that lesson with -VoxelGIVis).
	//
	// Optional companions:
	//   -VoxelGIConvergeSettle=<s>  seconds to let streaming/voxelization fill
	//                               the field before measuring (default 40).
	//   -VoxelGIConvergeLegacy      gather the bounce from the live AvgIrr, i.e.
	//                               the pre-fix behaviour, so one build can
	//                               produce both the drift and the fix.
	// -VoxelGILegacyBasis: command line rather than -ExecCmds for the same
	// reason as -VoxelGIVis -- it has to be in force before the first solve,
	// and a cvar set after init leaves already-solved bricks on the new basis
	// and silently produces a mixed capture.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelGILegacyBasis")))
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GI.LegacyConeBasis")))
		{
			V->Set(1, ECVF_SetByCode);
			UE_LOG(LogVoxelGI, Log, TEXT("VoxelGILegacyBasis: voxel.GI.LegacyConeBasis=1 (degenerate pre-fix basis)"));
		}
	}

	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelGIRelightAfter="), RelightAfterSeconds) && RelightAfterSeconds > 0.f)
	{
		bRelightArmed = true;
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GI.Enabled")))
		{
			V->Set(1, ECVF_SetByCode); // the harness is meaningless with GI off
		}
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VoxelGIRelightAfter: carving and timing a full relight at %.0fs. GI forced on."),
		       RelightAfterSeconds);
	}

	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelGIConverge="), ConvergePasses) && ConvergePasses > 0)
	{
		ConvergePasses = FMath::Clamp(ConvergePasses, 1, 64);
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGIConvergeSettle="), ConvergeSettleSeconds);
		bConvergeLegacy = FParse::Param(FCommandLine::Get(), TEXT("VoxelGIConvergeLegacy"));
		bConvergeSeed = FParse::Value(FCommandLine::Get(), TEXT("VoxelGIConvergeSeed="), ConvergeSeedValue);
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GI.Enabled")))
		{
			V->Set(1, ECVF_SetByCode); // the harness is meaningless with GI off
		}
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VoxelGIConverge: %d passes after %.0fs settle (legacy in-place gather=%d). GI forced on."),
		       ConvergePasses, ConvergeSettleSeconds, bConvergeLegacy ? 1 : 0);
	}
	else
	{
		ConvergePasses = 0;
	}
}

void UVoxelGISubsystem::Deinitialize()
{
	ClearAllState();
	Field.Reset();
	Super::Deinitialize();
}

bool UVoxelGISubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UVoxelGISubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelGISubsystem, STATGROUP_Tickables);
}

bool UVoxelGISubsystem::IsTickable() const
{
	// Zero per-frame cost when GI is off. bHasState keeps ticking for exactly
	// as long as it takes to release the field after a runtime toggle-off.
	//
	// ArmRequestedEnabled keeps it tickable through a -VoxelGIOff leg, which
	// would otherwise never tick at all and so could never verify that its own
	// arm held. That is the one case where the verification matters most: a
	// silently re-enabled control arm is indistinguishable from a clean one in
	// every number it produces. Costs a cvar read and a clock compare per frame,
	// and ONLY on a leg that passed an arm switch (-1 otherwise).
	return VoxelGI::IsEnabled() || bHasState || ArmRequestedEnabled >= 0;
}

void UVoxelGISubsystem::ClearAllState()
{
	if (Field)
	{
		Field->Reset();
	}
	PendingVoxelize.Reset();
	PendingPooledVoxelize.Reset();
	DirtyQueue.Reset();
	DirtySet.Reset();
	// P7 march arm. Cleared with the rest, and for the same reason: a key names
	// a brick coordinate in a world that is going away. Carrying one across a
	// teardown would dispatch a march at a coordinate the next world does not
	// have, which writes zeros into a live texel range -- a rectangle of
	// unlit terrain, not an error.
	MarchDirtyQueue.Reset();
	MarchDirtySet.Reset();
	RefreshQueue.Reset();
	RefreshSet.Reset();
	BrickComponents.Reset();
	RefreshRotation.Reset();
	RefreshCursor = 0;
	VolumeUploadQueue.Reset();
	VolumeUploadSet.Reset();
	// An in-flight re-centre must not survive a toggle-off: its row buckets name
	// bricks the field is about to drop, and resuming it later would restage the
	// volume from a resident set that no longer exists.
	bVolumeRecentring = false;
	RecentreRowBricks.Reset();
	// The local splat queue names texels against an origin and a light list that a
	// toggle-off may outlive, so it is dropped and the feature re-arms from scratch
	// (bLocalLightsActive false forces the full re-splat in TickVolume). The
	// REGISTERED LIGHTS themselves survive: they are the caller's state, not this
	// module's, and a torch the player lit does not stop existing because a
	// rendering cvar moved.
	LocalSplatQueue.Reset();
	bLocalLightsActive = false;
	// The shadow and the origin deliberately SURVIVE a toggle-off: the GPU
	// texture still holds those bytes, and throwing the mirror away would leave
	// the harness comparing against nothing while the volume kept rendering.
	// Re-centring picks up from wherever the origin was left.
	bCoarseDirty = false;
	bHasState = false;

	// PUBLISH Enabled=0 TO THE SHADER ON THE WAY DOWN.
	//
	// TickVolume has an identical publish in its own disabled branch, and that
	// branch is UNREACHABLE on a voxel.GI.Enabled toggle-off: Tick returns at
	// the !IsEnabled() guard before TickVolume is ever called. So without this,
	// a mid-session `voxel.GI.Enabled 0` left the last Enabled=1 uniform buffer
	// bound with its stale texels, and the pooled factory kept sampling a
	// frozen light field forever -- GI "off" that still costs a volume sample
	// per pixel and still tints the world with whatever it last solved.
	//
	// That matters most for exactly the arm this exists to serve. -VoxelGIOff
	// sets the cvar at GameMode init, before the first tick, so ClearAllState
	// never runs and the buffer is still InitRHI's Enabled=0 default -- that
	// path was always clean. The contaminated path is the -ExecCmds toggle,
	// which is the one a reader reaches for first and the one this subsystem's
	// own comments already warn lands mid-stream. A control arm that silently
	// keeps sampling the feature it is controlling is the same failure shape as
	// Defect 1 in armA-drawpath-ceiling-2026-08-19.txt, and it is cheaper to
	// make both routes correct than to document which one is safe.
	//
	// Guarded on bVolumeSettingsValid so a session that never turned the volume
	// on does not enqueue a render command to disable something never enabled.
	if (bVolumeSettingsValid && LastVolumeSettings.bEnabled)
	{
		FVoxelGIVolumeSettings Off = LastVolumeSettings;
		Off.bEnabled = false;
		Off.bLocalEnabled = false;
		LastVolumeSettings = Off;
		ENQUEUE_RENDER_COMMAND(VoxelGIVolumeOffOnClear)(
			[Off](FRHICommandListImmediate&) { GVoxelGIVolume.UpdateParameters_RenderThread(Off); });
	}
}

// --- ingest ----------------------------------------------------------------

void UVoxelGISubsystem::NotifyChunkMeshUpdated(UVoxelChunkComponent* Component)
{
	// First branch, before anything else: with GI off this is one cvar read
	// per SetChunkQuads call and nothing more.
	if (!VoxelGI::IsEnabled() || !Component)
	{
		return;
	}
	// Only level-0 chunks feed the field. Coarse rings are the LOD story:
	// beyond R0 there is no light field and chunks shade with the mesher's
	// geometric AO exactly as they do today.
	if (Component->GetLevel() != 0)
	{
		return;
	}
	// P7 MARCH ARM. The KEY is the payload here; the quads are ignored. Placed
	// before the retire test below because that test returns, and on this arm
	// the notification is the only thing that tells the march a brick changed.
	if (VoxelGI::IsMarchBricksEnabled())
	{
		PushMarchDirty(FVoxelLightField::WorldToBrick(Component->GetComponentLocation()));
		++StatIngestDroppedByArm;
		bHasState = true;
		return;
	}
	// P7-c ARM. The COMPONENT path. Gated here as well as in WantsChunkQuads
	// because these are two different demands: WantsChunkQuads governs the
	// pooled path's readback, and this hook is the only feed under
	// voxel.Stream.GPU 0. Gating only the first would leave the component arm
	// still voxelizing from quads while reporting quadsRetainedForGI = 0 --
	// an arm that reads as applied and is half applied.
	if (VoxelGI::IsQuadIngestRetired())
	{
		++StatIngestDroppedByArm;
		return;
	}
	if (PendingVoxelize.Num() >= kMaxPendingVoxelize)
	{
		// Overflow (a very large simultaneous edit): drop the oldest rather
		// than growing without bound. The dropped chunk is picked up again by
		// the round-robin refresh, just later.
		PendingVoxelize.RemoveAt(0, 1, EAllowShrinking::No);
	}
	PendingVoxelize.Add(Component);
	bHasState = true;
}

void UVoxelGISubsystem::NotifyPooledChunkMeshUpdated(const FVector& ChunkOriginUU, int32 ChunkLevel,
                                                     TArray<FVoxelChunkQuad>&& Quads)
{
	// First branch, before anything else and BEFORE the move: with GI off this
	// is one cvar read per pooled apply and nothing more -- no queue growth, no
	// quad copy, and the caller's array is left exactly as it was.
	if (!VoxelGI::IsEnabled())
	{
		return;
	}
	// Level-0 only, matching NotifyChunkMeshUpdated and the scene proxy's
	// bGIEnabled = VoxelGI::IsEnabled() && ChunkLevel == 0. Coarse rings have
	// no field coverage on either renderer.
	if (ChunkLevel != 0)
	{
		return;
	}
	// P7 MARCH ARM, AND IT MUST COME BEFORE THE EMPTY-QUAD TEST.
	//
	// This is the whole reason the march arm needs no new plumbing: with
	// WantsChunkQuads returning false the chunk goes DIRECT TO POOL and arrives
	// here with an EMPTY quad array -- so the key is delivered for free, on the
	// existing hook, by the very path the arm exists to preserve. Testing
	// Quads.Num() == 0 first would drop every one of them.
	if (VoxelGI::IsMarchBricksEnabled())
	{
		PushMarchDirty(FVoxelLightField::WorldToBrick(ChunkOriginUU));
		++StatIngestDroppedByArm;
		bHasState = true;
		return;
	}
	if (Quads.Num() == 0)
	{
		return;
	}
	// P7-c ARM. The POOLED path. In principle unreachable with the arm on --
	// WantsChunkQuads returned false, so the chunk went direct-to-pool and
	// arrives with Quads empty, which the test above already dropped. Kept
	// anyway, and COUNTED, for the reason the `Quads.Num() == 0` test is worth
	// having twice: if this ever fires with the arm on, the demand did NOT go
	// away and the streaming refund this arm exists to measure is not what it
	// appears to be. A zero here is a real reading; a non-zero is a defect that
	// would otherwise be invisible.
	if (VoxelGI::IsQuadIngestRetired())
	{
		++StatIngestDroppedByArm;
		return;
	}
	if (PendingPooledVoxelize.Num() >= kMaxPendingVoxelize)
	{
		// Same overflow policy as the component queue: drop the oldest rather
		// than grow without bound. Costlier here because the entry owns its
		// quads, which is exactly why the bound matters.
		PendingPooledVoxelize.RemoveAt(0, 1, EAllowShrinking::No);
	}
	FPendingPooledChunk& Entry = PendingPooledVoxelize.AddDefaulted_GetRef();
	Entry.OriginUU = ChunkOriginUU;
	Entry.Quads = MoveTemp(Quads);
	bHasState = true;
}

bool UVoxelGISubsystem::WantsChunkQuads(const FVector& ChunkOriginUU, int32 ChunkLevel) const
{
	// Same first two tests as NotifyPooledChunkMeshUpdated, in the same order.
	if (!VoxelGI::IsEnabled())
	{
		return false;
	}
	if (ChunkLevel != 0)
	{
		return false;
	}

	// P7-c ARM. Placed AFTER the level-0 test so the candidate population is
	// unchanged between arms -- a candidate is still exactly "a level-0 chunk
	// that was offered", and the two arms' percentages are therefore over the
	// same denominator. The decline is counted in BOTH places on purpose:
	// StatQuadsDeclined keeps quadsRetainedForGI's "0 of N candidates" reading
	// truthful, and StatQuadsRetiredByArm says the zero was produced by the arm
	// rather than by a call site that stopped firing. Those two readings are
	// otherwise identical, and this project has already been fooled by that
	// exact pair once.
	if (VoxelGI::IsQuadIngestRetired())
	{
		++StatQuadsDeclined;
		++StatQuadsRetiredByArm;
		return false;
	}

	// The radius test the voxelize drain applies (VoxRejectedRadius), widened.
	//
	// FieldCentreUU is last tick's view origin, which is the right thing to
	// compare against and is why the margin is here: at 20 m/s the camera moves
	// ~4.5 m across the fork's measured ~225 ms submit->deliver, against 17.5 m
	// of slack at the default 7000 UU radius.
	//
	// WHAT THIS ACTUALLY COVERS TODAY, stated because it is easy to assume
	// otherwise: voxel.GI.RadiusUU defaults to 7000 UU, which was deliberately
	// larger than the OLD 64 m R0 ring (6400 UU). Since 2026-07-27 the shipped
	// R0 is 128 m (12,800 UU), so with GI on the radius covers only the inner
	// ~half of level 0: near-field level-0 chunks stay on the readback path for
	// GI ingest, and outer-R0 -- including the leading edge under motion, the
	// coverage-hole population -- goes direct along with levels 1-5. If
	// voxel.GI.RadiusUU is ever raised toward the new R0 edge, GI ingest volume
	// and readback traffic both grow with it. Both configurations are correct;
	// the gate just tracks the configuration instead of assuming one.
	const double RadiusUU = double(CVarGIRadiusUU.GetValueOnGameThread()) * 1.25;
	const bool bRetain = FVector::Dist(ChunkOriginUU, FieldCentreUU) <= RadiusUU;
	// BOTH counters, always. See the member declarations: `retained` alone
	// cannot tell "the gate ran and declined" from "the gate never ran", and a
	// silently unreached gate reads as a clean zero. Both zero over a second of
	// streaming means the instrument is broken, not that the cost is absent.
	//
	// Counted here rather than at the call site so this stays inside a file this
	// workstream owns, and so the count is of the DECISION rather than of a
	// caller's use of it.
	if (bRetain)
	{
		++StatQuadsRetainedForGI;
	}
	else
	{
		++StatQuadsDeclined;
	}
	return bRetain;
}

void UVoxelGISubsystem::PushDirty(const FIntVector& Key)
{
	bool bAlready = false;
	DirtySet.Add(Key, &bAlready);
	if (!bAlready)
	{
		DirtyQueue.Add(Key);
	}
}

void UVoxelGISubsystem::PushMarchDirty(const FIntVector& Key)
{
	bool bAlready = false;
	MarchDirtySet.Add(Key, &bAlready);
	if (bAlready)
	{
		return;
	}
	if (MarchDirtyQueue.Num() >= kMaxPendingVoxelize)
	{
		// Same overflow policy as the two voxelize queues, and cheaper here
		// because an entry is three ints rather than a quad list. The dropped
		// brick is not lost forever: anything that re-meshes queues again, and
		// on this arm a brick with no dispatch keeps whatever texels it had
		// rather than going black.
		MarchDirtySet.Remove(MarchDirtyQueue[0]);
		MarchDirtyQueue.RemoveAt(0, 1, EAllowShrinking::No);
	}
	MarchDirtyQueue.Add(Key);
}

// P7 march arm. Drain the dirty-brick queue into one GPU dispatch.
//
// EVERY POSITION IS RESOLVED HERE, IN DOUBLE, AND NOTHING DOWNSTREAM
// RE-DERIVES ONE. A GI cell coordinate at this world's scale is ~150,000 and
// float32 quantises it coarser than the 40 UU cell it addresses, so the shader
// is handed a corner already differenced against the march frame. Same seam,
// same reasoning, as VoxelShadowMarch.cpp differencing its ray origin in double
// before it hands over a float3.
void UVoxelGISubsystem::TickMarchBricks()
{
	if (MarchDirtyQueue.Num() == 0)
	{
		return;
	}
	// The volume must be anchored before a texel index means anything. Not an
	// error: EnsureVolumeOrigin retries every tick and the terrain pool is
	// created lazily (P7-a). Dispatching now would write texels addressed
	// against an origin that is about to change.
	if (!bVolumeOriginSet || VolumeDim <= 0 || !bFieldCentreResolved)
	{
		return;
	}

	const int32 Budget = FMath::Max(0, CVarGIMarchBricksPerFrame.GetValueOnGameThread());
	if (Budget == 0)
	{
		return;
	}

	FVoxelGIMarchRequest Request;
	Request.VolumeDim = VolumeDim;
	Request.CellSizeUU = float(VoxelLF::CellSizeUU);
	Request.ConeReachUU = CVarGIConeDistanceUU.GetValueOnGameThread();
	// One level-0 voxel of self-hit guard. The cone starts at a cell CENTRE and
	// the cell is four voxels across, so this cannot skip past a wall the cell
	// is touching -- it only avoids the voxel the origin is standing in.
	Request.StartOffsetUU = float(VoxelCoords::VoxelSizeUU);
	// The traversal's cone rule widens the sampled level with distance. There is
	// no coarser level to widen INTO inside ring 0 (see the .usf header), so a
	// slope of 0 asks for level 0 for the whole ray, which is what is actually
	// resident. A non-zero slope here would ask for bricks that do not exist and
	// get level 0 anyway, less obviously.
	Request.ConeSlopeUU = 0.0f;
	Request.SkyIntensity = 1.0f;
	Request.Walk = FMath::Clamp(CVarGIMarchWalk.GetValueOnGameThread(), 0, 2);
	Request.bStatsEnabled = CVarGIMarchStats.GetValueOnGameThread() != 0;

	// The step budget the marcher already ships, if it is there to read. The
	// GI cone is 3000 UU = 300 level-0 voxels, comfortably inside the 886 this
	// project sized for 51.2 m, so this is a shared setting rather than a new
	// knob -- and NOT one to raise to make a number look better.
	static const auto* CVarStepBudget =
		IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.March.StepBudget"));
	Request.StepBudget = CVarStepBudget ? FMath::Max(1, CVarStepBudget->GetInt()) : 886;

	// The cone basis, copied from its ONE definition. See the .usf header for
	// why this is uploaded rather than restated in HLSL.
	static_assert(VoxelGIMarch::kNumTraceDirs == VoxelLF::NumTraceDirs,
	              "GI march trace-direction count must match the light field's cone basis");
	static_assert(VoxelGIMarch::kNumSlots == VoxelLF::NumDirs,
	              "GI march slot count must match the ambient cube");
	static_assert(VoxelGIMarch::kBrickEdgeCells == VoxelLF::BrickEdgeCells,
	              "GI march brick edge must match the light field brick");
	for (int32 T = 0; T < VoxelLF::NumTraceDirs; ++T)
	{
		Request.TraceDir[T] = VoxelLF::TraceDirTable[T];
	}
	for (int32 S = 0; S < VoxelLF::NumDirs; ++S)
	{
		for (int32 T = 0; T < VoxelLF::NumTraceDirs; ++T)
		{
			Request.SlotWeight[S][T] = VoxelLF::SlotWeight[S][T];
		}
	}

	// ---- the march frame ---------------------------------------------------
	//
	// Constructed exactly as VoxelShadowMarch.cpp constructs its own: a pure
	// function of the pose, snapped DOWN to a 32-voxel chunk boundary so
	// chunk-coordinate arithmetic stays exact and sub-chunk camera jitter cannot
	// move the origin. Sized to hold BOTH populations this dispatch touches --
	// every cell in the volume, plus a cone of ConeReachUU fired from any of
	// them -- because a ray leaving the frame reads as empty space, and empty
	// space is exactly what makes GI too bright.
	const double VolumeHalfExtentUU = 0.5 * double(VolumeDim) * double(VoxelLF::CellSizeUU);
	const int32 ReachVoxels = FMath::CeilToInt(
		(VolumeHalfExtentUU + double(Request.ConeReachUU)) / double(VoxelCoords::VoxelSizeUU));
	const auto SnapDown = [](int64 V) { return int32((V >> 5) << 5); };
	const int64 CentreVoxelX = int64(FMath::FloorToDouble(FieldCentreUU.X / double(VoxelCoords::VoxelSizeUU)));
	const int64 CentreVoxelY = int64(FMath::FloorToDouble(FieldCentreUU.Y / double(VoxelCoords::VoxelSizeUU)));
	const int64 CentreVoxelZ = int64(FMath::FloorToDouble(FieldCentreUU.Z / double(VoxelCoords::VoxelSizeUU)));
	Request.FrameOriginVoxel = FIntVector(SnapDown(CentreVoxelX - ReachVoxels),
	                                      SnapDown(CentreVoxelY - ReachVoxels),
	                                      SnapDown(CentreVoxelZ - ReachVoxels));
	const FVector FrameOriginUU(double(Request.FrameOriginVoxel.X) * double(VoxelCoords::VoxelSizeUU),
	                            double(Request.FrameOriginVoxel.Y) * double(VoxelCoords::VoxelSizeUU),
	                            double(Request.FrameOriginVoxel.Z) * double(VoxelCoords::VoxelSizeUU));

	// ---- the batch ---------------------------------------------------------
	Request.Bricks.Reserve(FMath::Min(Budget, MarchDirtyQueue.Num()));
	while (Request.Bricks.Num() < Budget && MarchDirtyQueue.Num() > 0)
	{
		const FIntVector Key = MarchDirtyQueue[0];
		MarchDirtyQueue.RemoveAt(0, 1, EAllowShrinking::No);
		MarchDirtySet.Remove(Key);

		// The brick's own texel range, in the volume's cell lattice. A brick
		// outside the volume is DROPPED HERE rather than dispatched and dropped
		// in the shader -- the shader's own bounds test is the backstop, not the
		// policy, and dispatching work that is guaranteed to write nothing would
		// spend the frame's budget on it.
		const FIntVector TexelMin(Key.X * VoxelLF::BrickEdgeCells - VolumeCellOrigin.X,
		                          Key.Y * VoxelLF::BrickEdgeCells - VolumeCellOrigin.Y,
		                          Key.Z * VoxelLF::BrickEdgeCells - VolumeCellOrigin.Z);
		if (TexelMin.X < 0 || TexelMin.Y < 0 || TexelMin.Z < 0 ||
		    TexelMin.X + VoxelLF::BrickEdgeCells > VolumeDim ||
		    TexelMin.Y + VoxelLF::BrickEdgeCells > VolumeDim ||
		    TexelMin.Z + VoxelLF::BrickEdgeCells > VolumeDim)
		{
			continue;
		}

		const FVector CornerWorldUU(double(Key.X) * double(VoxelLF::BrickEdgeUU),
		                            double(Key.Y) * double(VoxelLF::BrickEdgeUU),
		                            double(Key.Z) * double(VoxelLF::BrickEdgeUU));

		FVoxelGIMarchBrick Brick;
		Brick.CornerLocalUU = FVector3f(CornerWorldUU - FrameOriginUU);
		Brick.TexelMin = TexelMin;
		Request.Bricks.Add(Brick);
	}

	if (Request.Bricks.Num() == 0)
	{
		return;
	}

	StatMarchBricksSubmitted += VoxelGIMarch::Enqueue_GameThread(MoveTemp(Request));
}

void UVoxelGISubsystem::MarkBrickNeighbourhoodDirty(const FIntVector& BrickCoord, int32 RadiusBricks)
{
	for (int32 DZ = -RadiusBricks; DZ <= RadiusBricks; ++DZ)
	for (int32 DY = -RadiusBricks; DY <= RadiusBricks; ++DY)
	for (int32 DX = -RadiusBricks; DX <= RadiusBricks; ++DX)
	{
		PushDirty(BrickCoord + FIntVector(DX, DY, DZ));
	}
}

FVector UVoxelGISubsystem::ResolveViewOriginUU(bool& bOutResolved) const
{
	// THE CAMERA CACHE MUST BE GUARDED, and this cost a leg.
	//
	// APlayerCameraManager::GetCameraLocation() reads CameraCachePrivate.POV,
	// which is EMPTY -- i.e. exactly FVector::ZeroVector -- until the camera
	// manager has updated at least once. A tickable world subsystem can and
	// does tick before that on frame 0. So a non-null PlayerCameraManager is
	// NOT evidence that its location means anything, and the old code treated
	// it as though it were: it returned (0,0,0) on the first tick while the
	// pawn was correctly posed at (-6144000, -6144000, 219258).
	//
	// The engine states the rule in its own canonical accessor,
	// APlayerController::GetPlayerViewPoint (PlayerController.cpp):
	//
	//   else if (PlayerCameraManager != NULL &&
	//       PlayerCameraManager->GetCameraCacheTime() > 0.f) // Whether camera
	//                                                        // was updated at
	//                                                        // least once
	//
	// -- and then falls back to the view target. This now applies the same
	// guard and then falls through to the pawn, which is what the ORIGINAL
	// intent of this function's fallback chain already was; only the condition
	// gating the first branch was missing.
	//
	// WHAT A ZERO CENTRE ACTUALLY BROKE, because it is more than one thing:
	//   1. the voxel.GI.Debug 1 line's camAboveSurface probe called
	//      GetSurfaceHeightUU(0, 0). That is a BLOCKING, footprint-requesting
	//      fine-tile query, the amplifier's column stencil around the origin
	//      reaches fine pixel (-2,-2), tile (-1,-1) is not baked for this seed,
	//      and the fine-tier gate is fatal on an unattended run. It killed
	//      g0-gion-r1 on frame 0. G1 never saw it because GI-off returns from
	//      Tick before the debug block.
	//   2. every distance test in the tick -- the voxelize drain's build-radius
	//      rejection and WantsChunkQuads -- measured from the origin instead of
	//      the camera, so on that tick every chunk is out of range.
	//   3. worst and quietest: EnsureVolumeOrigin LATCHES
	//      (bVolumeOriginSet) and derives VolumeOriginWorldUU from
	//      FieldCentreUU. Under voxel.Stream.GPU 1, where a pool component can
	//      already exist on frame 0, that anchors the GI volume 6,144 km from
	//      the camera. This leg escaped it only because voxel.Stream.GPU 0
	//      never creates a pool, so EnsureVolumeOrigin bailed before latching.
	//
	// bOutResolved is FALSE when nothing authoritative was available. Callers
	// must not perform world queries or latch anything from the returned value
	// in that case -- it is the PREVIOUS centre, carried forward.
	bOutResolved = true;
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (const APlayerCameraManager* Cam = PC->PlayerCameraManager)
			{
				if (Cam->GetCameraCacheTime() > 0.f)
				{
					return Cam->GetCameraLocation();
				}
			}
			// Camera not yet updated: the pawn is authoritative and correct
			// from the frame it is posed, which is why the fixture's spawn pose
			// is already right in the log line above the crash.
			if (const APawn* Pawn = PC->GetPawn())
			{
				return Pawn->GetActorLocation();
			}
		}
	}
	bOutResolved = false;
	return FieldCentreUU;
}

// --- GPU volume driver (docs/gpu-gi-volume-design.md §3) --------------------

void UVoxelGISubsystem::PushVolumeUpload(const FIntVector& Key)
{
	// Gated on the cvar rather than queued unconditionally: with the volume off
	// nothing drains, and the dedupe set is keyed by ABSOLUTE brick coord, so a
	// moving camera would grow it without bound. Toggling the volume on
	// mid-session starts from an empty queue and is refilled by the round-robin
	// re-solve (voxel.GI.RefreshBricksPerFrame) within a few seconds.
	if (!VoxelGIVolume::IsEnabled())
	{
		return;
	}
	bool bAlready = false;
	VolumeUploadSet.Add(Key, &bAlready);
	if (!bAlready)
	{
		VolumeUploadQueue.Add(Key);
	}
}

bool UVoxelGISubsystem::EnsureVolumeOrigin()
{
	if (bVolumeOriginSet)
	{
		return true;
	}
	// DO NOT LATCH A VOLUME ORIGIN FROM AN UNRESOLVED CENTRE.
	//
	// Everything below derives VolumeOriginWorldUU from FieldCentreUU and then
	// sets bVolumeOriginSet, which makes this function a no-op for the rest of
	// the session. On frame 0 FieldCentreUU can still be the carried-forward
	// zero (see ResolveViewOriginUU), and under voxel.Stream.GPU 1 a pool
	// component may already exist -- so without this guard the GI volume would
	// anchor 6,144 km from the camera at the current test spawn and stay there
	// until a re-centre dragged it back, sampling irradiance from the wrong
	// place in the meantime. Deferring by one tick costs nothing: this is
	// already called every tick until it succeeds, and it already returns false
	// on the no-pool path for the same "try again next frame" reason.
	if (!bFieldCentreResolved)
	{
		return false;
	}

	// First-time establishment only. Camera-following re-centring afterwards is
	// BeginVolumeRecentre/StepVolumeRecentre (dead zone + staged re-upload + a
	// one-frame origin swap, docs/gpu-gi-volume-design.md §4).
	//
	// P7-a: IDENTIFY OR REFUSE. There is no third branch, and in particular
	// there is no "fall back to the first pool" branch -- a fallback would fire
	// exactly and only in the case where the right answer is unknown, which is
	// the case it must not guess in.
	int32 NumPoolCandidates = 0;
	const EFindTerrainPoolOutcome PoolOutcome =
		FindTerrainPoolWorldLocation(GetWorld(), PoolWorldUU, &NumPoolCandidates);
	if (PoolOutcome != EFindTerrainPoolOutcome::Found)
	{
		++PoolIdentityRefusals;
		if (PoolOutcome == EFindTerrainPoolOutcome::NameMismatch)
		{
			// THE ONE OUTCOME THAT IS A DEFECT RATHER THAN A WAIT. Two ends of
			// the same join disagree: the terrain subsystem handed over a pool
			// that does not carry the terrain name. Loud, once, with both
			// sides printed so the stale end is identifiable from the log
			// alone.
			if (!bLoggedPoolNameMismatch)
			{
				bLoggedPoolNameMismatch = true;
				const UVoxelWorldSubsystem* Terrain =
					GetWorld() ? GetWorld()->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
				const UVoxelGpuPoolComponent* Pool = Terrain ? Terrain->GetTerrainGpuPool() : nullptr;
				UE_LOG(LogVoxelGI, Error,
				       TEXT("VoxelGI volume: POOL IDENTITY REFUSED -- the terrain subsystem's own pool is ")
				       TEXT("named '%s' but this build expects '%s'. The GI volume is NOT anchored and no ")
				       TEXT("texels are uploaded; GI will read as absent rather than as misplaced, which is ")
				       TEXT("the safe direction. One of the two ends of the name join is stale: producer is ")
				       TEXT("VoxelWorldSubsystem.cpp GetOrCreateGpuPool -> SetPoolName, consumer is this ")
				       TEXT("function. %d registered pool component(s) in this world. ")
				       TEXT("voxel.GI.PoolIdentityMutate=%d -- if that is 1 this refusal is DELIBERATE and ")
				       TEXT("is the gate proving itself, not a defect."),
				       Pool ? *Pool->GetPoolName() : TEXT("<no pool>"),
				       VoxelGpuPool::kTerrainPoolName, NumPoolCandidates,
				       CVarGIPoolIdentityMutate.GetValueOnGameThread());
			}
			return false;
		}
		// The two benign outcomes. Both mean "not yet", both are retried every
		// tick, and both were already the pre-P7-a behaviour on the no-pool
		// path. Logged once, and they now say WHICH of the two it was.
		if (!bLoggedNoPool)
		{
			bLoggedNoPool = true;
			UE_LOG(LogVoxelGI, Log,
			       TEXT("VoxelGI volume: %s -- texel uploads deferred, retried every tick. ")
			       TEXT("(voxel.Stream.GPU 0 never creates a terrain pool, and the volume has no consumer ")
			       TEXT("there.) %d registered pool component(s) in this world at this moment."),
			       PoolOutcome == EFindTerrainPoolOutcome::NoWorldSubsystem
			           ? TEXT("no UVoxelWorldSubsystem in this world")
			           : TEXT("the terrain GPU pool has not been created yet"),
			       NumPoolCandidates);
		}
		// ESCALATION. "Deferred, retried every tick" is the right message for
		// the seconds before the first chunk lands and the WRONG one forever
		// after -- and logged once at Log severity the two are identical in a
		// log file. 1800 refusals is ~30 s at 60 Hz, far past any startup.
		//
		// The named suspect is deliberate: under voxel.Terrain.RetireQuads,
		// ApplyMeshResult returns on NumQuads == 0 and never reaches
		// GetOrCreateGpuPool, so the pool is never created and this wait never
		// ends. GI then reads as absent -- for WATER too -- with no error
		// anywhere, because the shader gates on bInsideVolume.
		if (!bLoggedNoPoolEscalated && PoolIdentityRefusals > 1800)
		{
			bLoggedNoPoolEscalated = true;
			UE_LOG(LogVoxelGI, Error,
			       TEXT("VoxelGI volume: STILL NOT ANCHORED after %lld refusals (~30 s). This is no "
			            "longer a startup wait -- GI is ABSENT, for water as well as terrain, and "
			            "the shader gates on bInsideVolume so nothing else will report it. Most "
			            "likely cause: the terrain GPU pool is never created because "
			            "voxel.Terrain.RetireQuads makes ApplyMeshResult return before "
			            "GetOrCreateGpuPool. %d registered pool component(s) in this world."),
			       (long long)PoolIdentityRefusals, NumPoolCandidates);
		}
		return false;
	}

	// SAY IT OUT LOUD, ONCE, AT THE MOMENT THE ORIGIN IS LATCHED -- and say it
	// on the SUCCESS path, not only on the suspicious one.
	//
	// The previous version logged a Warning when the census exceeded one and
	// nothing at all otherwise, so a leg that anchored correctly and a leg that
	// anchored to a water bucket both produced silence whenever exactly one
	// pool happened to be registered at that instant. This line always prints,
	// always names the pool it chose, and always reports how many candidates it
	// declined -- so "GI is anchored to the terrain pool" is a fact a leg can
	// be grepped for rather than an absence that has to be trusted.
	//
	// GREP: "VoxelGI volume: ANCHORED"
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VoxelGI volume: ANCHORED to the TERRAIN pool '%s' at (%.0f, %.0f, %.0f); ")
	       TEXT("%d registered pool component(s) in this world, %d declined by identity. ")
	       TEXT("Selection is by OWNERSHIP (UVoxelWorldSubsystem::GetTerrainGpuPool) cross-checked ")
	       TEXT("against the pool NAME -- never by TObjectIterator order."),
	       VoxelGpuPool::kTerrainPoolName, PoolWorldUU.X, PoolWorldUU.Y, PoolWorldUU.Z,
	       NumPoolCandidates, FMath::Max(0, NumPoolCandidates - 1));

	VolumeDim = VoxelGIVolume::GetDim();
	if (VolumeDim <= 0)
	{
		return false;
	}

	// Brick-snap, in DOUBLE (§4). Everything downstream leans on texel i being
	// world Origin + (i+0.5)*40 with the same lattice the field uses: get this
	// wrong and every sample is shifted half a cell, which reads as a soft
	// lighting offset rather than as a bug.
	const double HalfExtentUU = 0.5 * double(VolumeDim) * double(VoxelLF::CellSizeUU);
	auto SnapDownToBrick = [](double V) -> double
	{
		return FMath::FloorToDouble(V / double(VoxelLF::BrickEdgeUU)) * double(VoxelLF::BrickEdgeUU);
	};
	VolumeOriginWorldUU = FVector(SnapDownToBrick(FieldCentreUU.X - HalfExtentUU),
	                              SnapDownToBrick(FieldCentreUU.Y - HalfExtentUU),
	                              SnapDownToBrick(FieldCentreUU.Z - HalfExtentUU));
	VolumeCellOrigin = FIntVector(int32(VolumeOriginWorldUU.X / VoxelLF::CellSizeUU),
	                              int32(VolumeOriginWorldUU.Y / VoxelLF::CellSizeUU),
	                              int32(VolumeOriginWorldUU.Z / VoxelLF::CellSizeUU));

	// POOL-PRIMITIVE SPACE, subtracted in double and narrowed exactly once
	// (§6). Passing the world-space origin as an FVector3f is explicitly
	// prohibited: at ~8.4M UU float32's ULP is 1.0 UU against a 40 UU cell.
	const FVector3f OriginPoolUU = FVector3f(VolumeOriginWorldUU - PoolWorldUU);
	CommittedOriginPoolUU = OriginPoolUU;
	CommittedVolumeOriginWorldUU = VolumeOriginWorldUU;

	// ONE-SHOT DIAGNOSTIC: is this origin correct for the pools that actually
	// consume it?
	//
	// FVoxelGIVolume is a TGlobalResource -- ONE instance -- and this single
	// OriginPoolUU is bound identically to EVERY FVoxelQuadVertexFactory draw
	// (VoxelQuadVertexFactory.cpp:225), including all of water's buckets. But it
	// is computed against the TERRAIN pool's location, and each water bucket
	// carries its own unrelated WaterPoolBucketRebase. Neither uniform struct has
	// a per-primitive origin term, so a water vertex evaluates
	// (ProbePos - OriginPoolUU) with ProbePos in ITS bucket's space and the
	// origin in terrain's -- an error of exactly (bucketRebase - terrainRebase).
	//
	// VoxelGIVolume.h:69-74 states the assumption in the SINGULAR ("the pool
	// component carries the big offset"). That was true when written, before
	// water was split into buckets.
	//
	// If the deltas below are non-zero, every non-terrain pool is sampling GI
	// kilometres away from itself and bInsideVolume is false essentially always
	// -- water receives no GI, with no error anywhere. Printed once because it is
	// a structural question, not a per-frame one.
	{
		static bool bLoggedGiOriginAudit = false;
		if (!bLoggedGiOriginAudit)
		{
			bLoggedGiOriginAudit = true;
			for (TObjectIterator<UVoxelGpuPoolComponent> It; It; ++It)
			{
				const UVoxelGpuPoolComponent* P = *It;
				if (P == nullptr || !P->IsRegistered())
				{
					continue;
				}
				const FVector ThisPoolWorld = P->GetComponentLocation();
				const FVector Delta = ThisPoolWorld - PoolWorldUU;
				UE_LOG(LogVoxelGI, Warning,
				       TEXT("GI ORIGIN AUDIT: pool '%s' at (%.0f,%.0f,%.0f) | GI origin was "
				            "computed against (%.0f,%.0f,%.0f) | delta (%.0f,%.0f,%.0f) = %.0f UU. "
				            "%s"),
				       *P->GetPoolName(),
				       ThisPoolWorld.X, ThisPoolWorld.Y, ThisPoolWorld.Z,
				       PoolWorldUU.X, PoolWorldUU.Y, PoolWorldUU.Z,
				       Delta.X, Delta.Y, Delta.Z, Delta.Size(),
				       Delta.IsNearlyZero()
				           ? TEXT("SAME SPACE -- this pool's GI lookup is correct.")
				           : TEXT("DIFFERENT SPACE -- this pool samples GI at the wrong place by "
				                  "that distance, and bInsideVolume will be false. No error is "
				                  "raised anywhere; the symptom is simply no GI."));
			}
		}
	}

	VolumeShadow.Empty();
	VolumeShadow.SetNumZeroed(int64(VolumeDim) * VolumeDim * VolumeDim * 4);
	VolumeShadowNeg.Empty();
	VolumeShadowNeg.SetNumZeroed(int64(VolumeDim) * VolumeDim * VolumeDim * 4);

	// VolumeLocal's mirror. SIZED FROM ITS OWN DIM: the two volumes cover the same
	// box at different resolutions, so reusing VolumeDim here would allocate 8x too
	// much at the defaults and index wrongly at every other pair of dims. Allocated
	// only when the feature is on -- 3.4 MB of resident CPU memory nothing reads is
	// the same objection the texture's lazy allocation answers.
	VolumeLocalDim = VoxelGIVolume::GetLocalDim();
	if (VoxelGIVolume::IsLocalEnabled() && VolumeLocalDim > 0)
	{
		VolumeLocalShadow.Empty();
		VolumeLocalShadow.SetNumZeroed(int64(VolumeLocalDim) * VolumeLocalDim * VolumeLocalDim * 4);
	}
	bVolumeOriginSet = true;

	UE_LOG(LogVoxelGI, Log,
	       TEXT("VoxelGI volume: origin set dim=%d fieldCentre=(%.0f,%.0f,%.0f) originWorld=(%.0f,%.0f,%.0f) ")
	       TEXT("poolWorld=(%.0f,%.0f,%.0f) originPool=(%.1f,%.1f,%.1f) coverage=+/-%.0f UU shadow=%.1f MB"),
	       VolumeDim, FieldCentreUU.X, FieldCentreUU.Y, FieldCentreUU.Z,
	       VolumeOriginWorldUU.X, VolumeOriginWorldUU.Y, VolumeOriginWorldUU.Z,
	       PoolWorldUU.X, PoolWorldUU.Y, PoolWorldUU.Z,
	       OriginPoolUU.X, OriginPoolUU.Y, OriginPoolUU.Z,
	       HalfExtentUU, 2.0 * double(VolumeShadow.Num()) / (1024.0 * 1024.0));
	// Every cvar this feature reads, resolved, on one line. The local volume shares
	// the origin above, so its own numbers belong beside them rather than in a log
	// line somewhere else that has to be correlated by timestamp.
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VoxelGI local lights: voxel.GI.LocalLights=%d voxel.GI.LocalVolumeDim=%d (texel %.0f UU, ")
	       TEXT("mirror %.1f MB) voxel.GI.LocalLightOcclusion=%d voxel.GI.MaxLocalUploadsPerFrame=%d ")
	       TEXT("voxel.GI.LocalLightDeferred=%d"),
	       VoxelGIVolume::IsLocalEnabled() ? 1 : 0, VolumeLocalDim, LocalTexelUU(),
	       double(VolumeLocalShadow.Num()) / (1024.0 * 1024.0),
	       CVarGILocalLightOcclusion.GetValueOnGameThread(),
	       CVarGIMaxLocalUploadsPerFrame.GetValueOnGameThread(),
	       CVarGILocalLightDeferred.GetValueOnGameThread());

	// The uniform buffer is published by PushVolumeParamsIfChanged, which runs
	// straight after this in TickVolume and now owns every input to it.
	bVolumeSettingsValid = false;
	return true;
}

// --- camera-following re-centring (docs/gpu-gi-volume-design.md §4) ---------
//
// WHY NOT THE OTHER TWO. Toroidal addressing only uploads the new slabs, but
// hardware trilinear bleeds across the wrap plane and that plane SWEEPS THROUGH
// THE INTERIOR as the origin scrolls -- a 40 UU artifact plane moving across the
// world, fixable only by replacing the free trilinear with a manual 8-tap
// Load(). Double-buffering is 2x VRAM, which at the shipping size is another
// 67 MB. Both were considered and rejected in the design; this is the third
// option, and it costs one texture, no seam, and Dim^3*4 bytes of upload spread
// over a handful of frames per re-centre.
//
// THE ONE HONEST CAVEAT, which the design does not state and the implementation
// cannot remove: while the staged upload is in flight the texture holds a MIX of
// old-addressed and new-addressed bytes, and the shader is still reading it with
// the old origin. So the restaged region is transiently wrong -- by exactly the
// re-centre shift -- until the origin swaps. Two things make that not a pop, and
// both are deliberate rather than lucky:
//
//   * rows are restaged from BOTH ENDS INWARD, so the wrongness lives furthest
//     from the camera (where the distance fade is already attenuating GI) and
//     reaches the camera's own neighbourhood only in the last frame or two;
//   * the swap is atomic -- one uniform-buffer publish on one frame -- so there
//     is never a frame where part of the image uses one origin and part another.

bool UVoxelGISubsystem::VolumeNeedsRecentre() const
{
	if (!bVolumeOriginSet || VolumeDim <= 0 || bVolumeRecentring)
	{
		return false;
	}
	// Nothing resident means nothing to re-address, and the first seconds of a
	// session are exactly when the camera can be a long way from wherever the
	// origin was first established. Re-centring an empty field would spend a
	// whole staged upload writing zeros over zeros.
	if (!Field || Field->NumBricks() == 0)
	{
		return false;
	}
	const double HalfExtentUU = 0.5 * double(VolumeDim) * double(VoxelLF::CellSizeUU);
	const FVector Centre = VolumeOriginWorldUU + FVector(HalfExtentUU);

	// Clamped so the dead zone always leaves at least a two-brick margin of
	// volume beyond the camera. A dead zone wider than the half-extent would let
	// the camera leave coverage entirely without ever triggering a re-centre,
	// which presents as "GI stopped working over there" rather than as a bad
	// cvar value.
	const double MaxDeadZoneUU = FMath::Max(double(VoxelLF::BrickEdgeUU),
	                                        HalfExtentUU - 2.0 * double(VoxelLF::BrickEdgeUU));
	const double DeadZoneUU = FMath::Min(double(VoxelGIVolume::GetRecentreCells()) * double(VoxelLF::CellSizeUU),
	                                     MaxDeadZoneUU);

	return FMath::Abs(FieldCentreUU.X - Centre.X) > DeadZoneUU
	    || FMath::Abs(FieldCentreUU.Y - Centre.Y) > DeadZoneUU
	    || FMath::Abs(FieldCentreUU.Z - Centre.Z) > DeadZoneUU;
}

void UVoxelGISubsystem::BeginVolumeRecentre()
{
	if (!Field || VolumeDim <= 0 || VolumeShadow.Num() == 0)
	{
		return;
	}

	// Brick-snap in DOUBLE, exactly as EnsureVolumeOrigin does. The two must use
	// the same expression or the very first re-centre would shift the lattice by
	// a fraction of a cell and every sample after it would resample.
	const double HalfExtentUU = 0.5 * double(VolumeDim) * double(VoxelLF::CellSizeUU);
	auto SnapDownToBrick = [](double V) -> double
	{
		return FMath::FloorToDouble(V / double(VoxelLF::BrickEdgeUU)) * double(VoxelLF::BrickEdgeUU);
	};
	const FVector NewOriginWorldUU(SnapDownToBrick(FieldCentreUU.X - HalfExtentUU),
	                               SnapDownToBrick(FieldCentreUU.Y - HalfExtentUU),
	                               SnapDownToBrick(FieldCentreUU.Z - HalfExtentUU));
	if (NewOriginWorldUU.Equals(VolumeOriginWorldUU, 0.5))
	{
		return; // dead zone tripped but the snap lands on the same brick
	}

	const FIntVector OldCellOrigin = VolumeCellOrigin;
	VolumeOriginWorldUU = NewOriginWorldUU;
	VolumeCellOrigin = FIntVector(int32(FMath::FloorToDouble(VolumeOriginWorldUU.X / VoxelLF::CellSizeUU)),
	                              int32(FMath::FloorToDouble(VolumeOriginWorldUU.Y / VoxelLF::CellSizeUU)),
	                              int32(FMath::FloorToDouble(VolumeOriginWorldUU.Z / VoxelLF::CellSizeUU)));

	// Bucket the resident bricks that land inside the NEW volume by brick row, so
	// restaging a row is a lookup rather than a rescan. Everything else in the
	// volume legitimately becomes A=0 -- "no data", which the shader turns into
	// plain AO -- because there is no resident brick there to say otherwise.
	const int32 E = VoxelLF::BrickEdgeCells;
	const int32 Rows = VolumeDim / E;
	RecentreRowBricks.Reset();
	RecentreRowBricks.SetNum(Rows);

	TArray<FIntVector> Keys;
	Field->GetResidentKeys(Keys);
	int32 Inside = 0;
	for (const FIntVector& Key : Keys)
	{
		const FIntVector Base(Key.X * E - VolumeCellOrigin.X,
		                      Key.Y * E - VolumeCellOrigin.Y,
		                      Key.Z * E - VolumeCellOrigin.Z);
		if (Base.X < 0 || Base.Y < 0 || Base.Z < 0 ||
		    Base.X + E > VolumeDim || Base.Y + E > VolumeDim || Base.Z + E > VolumeDim)
		{
			continue;
		}
		RecentreRowBricks[Base.Z / E].Add(Key);
		++Inside;
	}

	// The per-brick queue is redundant now: every resident brick inside the new
	// volume is about to be re-encoded from the live field. Anything solved
	// DURING the re-centre re-queues itself and drains normally afterwards.
	VolumeUploadQueue.Reset();
	VolumeUploadSet.Reset();
	// Same for the local splat queue, and for a sharper version of the same
	// reason: its entries are TEXEL INDICES, so after the origin re-snaps they name
	// a different piece of the world than the light that queued them. Every one of
	// them is about to be rewritten by RestageVolumeZRange's own local pass, which
	// walks the whole volume against the new staging origin.
	LocalSplatQueue.Reset();

	bVolumeRecentring = true;
	RecentreLoRow = 0;
	RecentreHiRow = Rows;
	RecentreFrames = 0;
	RecentreCount = Inside;
	RecentreStartSeconds = FPlatformTime::Seconds();
	RecentreStepOccupied = 0;
	RecentreOccupiedBeforeLast = 0;
	RecentreTotalOccupied = 0;
	RecentreStepNearestUU = TNumericLimits<double>::Max();
	RecentreNearestBeforeLastUU = TNumericLimits<double>::Max();

	UE_LOG(LogVoxelGI, Log,
	       TEXT("VoxelGI volume: re-centre BEGIN camera=(%.0f,%.0f,%.0f) originWorld (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f) ")
	       TEXT("shift=(%d,%d,%d) cells | %d/%d resident bricks inside | staging %d rows"),
	       FieldCentreUU.X, FieldCentreUU.Y, FieldCentreUU.Z,
	       double(OldCellOrigin.X) * VoxelLF::CellSizeUU, double(OldCellOrigin.Y) * VoxelLF::CellSizeUU,
	       double(OldCellOrigin.Z) * VoxelLF::CellSizeUU,
	       VolumeOriginWorldUU.X, VolumeOriginWorldUU.Y, VolumeOriginWorldUU.Z,
	       VolumeCellOrigin.X - OldCellOrigin.X, VolumeCellOrigin.Y - OldCellOrigin.Y,
	       VolumeCellOrigin.Z - OldCellOrigin.Z,
	       Inside, Keys.Num(), Rows);
}

void UVoxelGISubsystem::RestageVolumeZRange(int32 Z0, int32 Z1)
{
	if (!Field || Z1 <= Z0)
	{
		return;
	}
	const int32 E = VoxelLF::BrickEdgeCells;
	const int64 SliceBytes = int64(VolumeDim) * VolumeDim * 4;

	// 1) Zero the range. Everything not covered by a resident brick MUST read
	//    A=0: it is the "no data -> plain AO" case, and leaving the previous
	//    occupant's bytes there would light new geometry with old irradiance
	//    from somewhere else entirely.
	FMemory::Memzero(VolumeShadow.GetData() + int64(Z0) * SliceBytes,
	                 int64(Z1 - Z0) * SliceBytes);
	FMemory::Memzero(VolumeShadowNeg.GetData() + int64(Z0) * SliceBytes,
	                 int64(Z1 - Z0) * SliceBytes);

	// 2) Re-encode the resident bricks whose rows fall in this range, under ONE
	//    read scope on the GAME thread (§1: never read the field from the render
	//    thread).
	{
		const FVoxelLightField::FReadScope Read(*Field);
		uint8 BrickPos[FVoxelLightField::BrickTexelBytes];
		uint8 BrickNeg[FVoxelLightField::BrickTexelBytes];
		for (int32 Row = Z0 / E; Row < Z1 / E; ++Row)
		{
			if (!RecentreRowBricks.IsValidIndex(Row))
			{
				continue;
			}
			for (const FIntVector& Key : RecentreRowBricks[Row])
			{
				Read.EncodeBrick(Key, BrickPos, BrickNeg);
				const FIntVector Base(Key.X * E - VolumeCellOrigin.X,
				                      Key.Y * E - VolumeCellOrigin.Y,
				                      Key.Z * E - VolumeCellOrigin.Z);
				for (int32 Z = 0; Z < E; ++Z)
				for (int32 Y = 0; Y < E; ++Y)
				{
					const int64 DstOffset = ((int64(Base.Z + Z) * VolumeDim + (Base.Y + Y)) * VolumeDim + Base.X) * 4;
					const int32 SrcOffset = (Z * E + Y) * E * 4;
					FMemory::Memcpy(VolumeShadow.GetData() + DstOffset, BrickPos + SrcOffset, E * 4);
					FMemory::Memcpy(VolumeShadowNeg.GetData() + DstOffset, BrickNeg + SrcOffset, E * 4);
				}
			}
		}
	}

	// 3) One whole-XY-slab upload for the range. Full-width rows have zero
	//    staging waste (§0's 256-byte row pitch), and a contiguous Z range is
	//    contiguous in the shadow, so this is one UpdateTexture3D for the lot.
	TArray<uint8> PayloadPos;
	TArray<uint8> PayloadNeg;
	PayloadPos.SetNumUninitialized(int64(Z1 - Z0) * SliceBytes);
	PayloadNeg.SetNumUninitialized(int64(Z1 - Z0) * SliceBytes);
	FMemory::Memcpy(PayloadPos.GetData(), VolumeShadow.GetData() + int64(Z0) * SliceBytes,
	                int64(Z1 - Z0) * SliceBytes);
	FMemory::Memcpy(PayloadNeg.GetData(), VolumeShadowNeg.GetData() + int64(Z0) * SliceBytes,
	                int64(Z1 - Z0) * SliceBytes);

	// TRANSIENT ACCOUNTING (voxel.GI.Debug >= 2). This is what turns "no lighting
	// pop" from an assertion into a number: every row restaged BEFORE the origin
	// swap is, for the frames until that swap, being read with the old origin and
	// therefore returning irradiance from a point one re-centre shift away. How
	// bad that is depends on two things this measures -- how many of those texels
	// carry data at all (most of the volume is A=0 rock/sky/unloaded, and an
	// A=0 texel reads as plain AO both before and after, so it cannot pop), and
	// how far the nearest of them is from the camera.
	if (VoxelGI::GetDebugLevel() >= 2)
	{
		int64 Occupied = 0;
		const uint8* Base = VolumeShadow.GetData() + int64(Z0) * SliceBytes;
		const int64 Count = int64(Z1 - Z0) * int64(VolumeDim) * VolumeDim;
		for (int64 I = 0; I < Count; ++I)
		{
			if (Base[I * 4 + 3] != 0) { ++Occupied; }
		}
		RecentreStepOccupied = Occupied;
		RecentreOccupiedBeforeLast += Occupied;

		const double RowMinZ = VolumeOriginWorldUU.Z + double(Z0) * VoxelLF::CellSizeUU;
		const double RowMaxZ = VolumeOriginWorldUU.Z + double(Z1) * VoxelLF::CellSizeUU;
		const double DistZ = FMath::Max(0.0, FMath::Max(RowMinZ - FieldCentreUU.Z, FieldCentreUU.Z - RowMaxZ));
		RecentreStepNearestUU = FMath::Min(RecentreStepNearestUU, DistZ);
	}

	const FIntVector Min(0, 0, Z0);
	const FIntVector Size(VolumeDim, VolumeDim, Z1 - Z0);
	ENQUEUE_RENDER_COMMAND(VoxelGIVolumeRestage)(
		[Min, Size, BytesPos = MoveTemp(PayloadPos), BytesNeg = MoveTemp(PayloadNeg)]
		(FRHICommandListImmediate& RHICmdList)
	{
		GVoxelGIVolume.UpdateTexels_RenderThread(RHICmdList, Min, Size,
		                                         BytesPos.GetData(), BytesNeg.GetData());
	});
	++VolumeRunsUploaded;

	// VolumeLocal RESTAGES IN THE SAME ROWS, IN THE SAME CALL, AND COMMITS ON THE
	// SAME FRAME AS ITS SIBLINGS. That is not tidiness: the three volumes share one
	// origin uniform, so a VolumeLocal that re-centred on its own schedule would be
	// addressed by an origin the shader is not using, displacing torch light by up
	// to the whole dead zone (2560 UU) mid-flight. Docs §5 item 7, and the
	// VOLUMECHECK local arm is what catches it if this line is ever moved out.
	RestageLocalZRange(Z0, Z1);
}

void UVoxelGISubsystem::StepVolumeRecentre()
{
	if (!bVolumeRecentring)
	{
		return;
	}
	const int32 E = VoxelLF::BrickEdgeCells;
	const int32 Rows = VolumeDim / E;
	// ~8 frames, the design's figure. Expressed as rows-per-frame so the wall
	// time of a re-centre does not scale with voxel.GI.VolumeDim.
	constexpr int32 kRecentreFrames = 8;
	const int32 RowsThisFrame = FMath::Max(2, FMath::DivideAndRoundUp(Rows, kRecentreFrames));

	// Snapshot what the transient looked like going INTO this frame. If this
	// frame turns out to be the committing one, these are the peak numbers --
	// the rows restaged in the committing frame are swapped in the same frame
	// they are written, so they are never read with the stale origin at all.
	RecentreNearestBeforeLastUU = RecentreStepNearestUU;
	const int64 OccupiedBeforeThisFrame = RecentreOccupiedBeforeLast;

	// FURTHEST FROM THE CAMERA FIRST, camera's own row LAST.
	//
	// This was "two rows off the low end, one off the high end" and that is a
	// measured defect, not a stylistic choice: taking an uneven number from each
	// end drifts the meeting point toward the high end instead of leaving it on
	// the camera, so the camera's own row was restaged in frame 7 of 8 and read
	// with the stale origin for a frame. Measured at
	// nearestStaleRowToCamera = 0 UU, which is exactly the artifact the ordering
	// exists to prevent.
	//
	// Picking the end by distance from the camera's row instead makes "the
	// camera's row is last" true by construction rather than by arithmetic
	// accident, and it stays true when the camera is off-centre in Z -- which it
	// usually is, because the dead zone lets it sit anywhere inside the box.
	const int32 CameraRow = FMath::Clamp(
		int32(FMath::FloorToDouble((FieldCentreUU.Z - VolumeOriginWorldUU.Z)
		                           / (double(VoxelLF::CellSizeUU) * double(E)))),
		0, Rows - 1);

	int32 Remaining = RowsThisFrame;
	while (Remaining > 0 && RecentreLoRow < RecentreHiRow)
	{
		// Whichever end is further from the camera goes now. The last row left
		// standing is therefore always the camera's own.
		const int32 LoDist = FMath::Abs(RecentreLoRow - CameraRow);
		const int32 HiDist = FMath::Abs((RecentreHiRow - 1) - CameraRow);
		if (LoDist >= HiDist)
		{
			RestageVolumeZRange(RecentreLoRow * E, (RecentreLoRow + 1) * E);
			++RecentreLoRow;
		}
		else
		{
			RestageVolumeZRange((RecentreHiRow - 1) * E, RecentreHiRow * E);
			--RecentreHiRow;
		}
		--Remaining;
	}
	++RecentreFrames;

	if (RecentreLoRow >= RecentreHiRow)
	{
		// THE ORIGIN SWAP. One publish, one frame, everything correct from here.
		bVolumeRecentring = false;
		RecentreRowBricks.Reset();
		CommittedOriginPoolUU = FVector3f(VolumeOriginWorldUU - PoolWorldUU);
		CommittedVolumeOriginWorldUU = VolumeOriginWorldUU;
		PushVolumeParamsIfChanged();
		RecentreTotalOccupied = RecentreOccupiedBeforeLast;
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VoxelGI volume: re-centre COMMIT after %d frames (%.1f ms wall), %d bricks restaged, ")
		       TEXT("originPool=(%.1f,%.1f,%.1f)"),
		       RecentreFrames, (FPlatformTime::Seconds() - RecentreStartSeconds) * 1000.0, RecentreCount,
		       CommittedOriginPoolUU.X, CommittedOriginPoolUU.Y, CommittedOriginPoolUU.Z);
		if (VoxelGI::GetDebugLevel() >= 2)
		{
			// The two numbers that decide whether the staged re-upload can pop:
			// how many occupied texels were read with the stale origin at the
			// worst moment (the frame before the commit), and how close to the
			// camera the nearest of them got. Rows written in the committing
			// frame are excluded because their upload and the origin swap are
			// enqueued in that order into the same render frame.
			UE_LOG(LogVoxelGI, Log,
			       TEXT("VOLUMERECENTRE transient: peakStaleTexels=%lld of %lld occupied (%.1f%%), ")
			       TEXT("nearestStaleRowToCamera=%.0f UU, fadeEnd=%.0f UU"),
			       (long long)OccupiedBeforeThisFrame, (long long)RecentreTotalOccupied,
			       RecentreTotalOccupied > 0 ? 100.0 * double(OccupiedBeforeThisFrame) / double(RecentreTotalOccupied) : 0.0,
			       RecentreNearestBeforeLastUU == TNumericLimits<double>::Max() ? -1.0 : RecentreNearestBeforeLastUU,
			       double(LastVolumeSettings.FadeEndUU));
		}
	}
}

void UVoxelGISubsystem::FlushVolume()
{
	int32 Guard = 0;
	while (bVolumeRecentring && Guard++ < 4096)
	{
		StepVolumeRecentre();
	}
	DrainVolumeUploads(-1);
	// The local splat queue too, and for the same reason: a harness that measures a
	// VolumeLocal which is merely BEHIND the light list measures the queue.
	DrainLocalSplats(-1);
}

void UVoxelGISubsystem::PushVolumeParamsIfChanged()
{
	FVoxelGIVolumeSettings New;
	New.bEnabled = VoxelGIVolume::IsEnabled() && bVolumeOriginSet;
	// The mirror's existence is part of the condition, not just the cvar: with no
	// mirror there has been no splat, so publishing LocalEnabled=1 would bind a
	// texture whose contents are the zero fill and claim it means something.
	New.bLocalEnabled = VoxelGIVolume::IsLocalEnabled() && bVolumeOriginSet && VolumeLocalShadow.Num() > 0;
	New.DebugVis = VoxelGIVolume::GetDebugVis();
	New.OriginPoolUU = CommittedOriginPoolUU;
	// CAMERA-RELATIVE, for consumers that have no pool primitive (the marcher) and
	// for the ones whose pool is the WRONG primitive (every water bucket).
	// Subtracted in double and narrowed once, same rule as the origin above.
	New.OriginRelCameraUU = FVector3f(CommittedVolumeOriginWorldUU - FieldCentreUU);
	// The CAMERA in pool space, not the volume centre: this is the quantity the
	// CPU shade measures its fade from (GICentreUU), and B4.3. Subtracted in
	// double and narrowed once, same rule as the origin.
	New.FadeCentrePoolUU = FVector3f(FieldCentreUU - PoolWorldUU);
	New.Strength = VoxelGI::GetStrength();
	New.AmbientFloor = VoxelGI::GetAmbientFloor();

	// Risk 8, enforced rather than documented. Beyond the volume face AM_Clamp
	// returns the edge texel, so a fade that has not finished by then does not
	// fade at all -- GI cuts off as a hard, plausible-looking lighting ring.
	// Clamping here means a fade that is too wide for the configured VolumeDim
	// degrades to "fades slightly early" instead.
	const float HalfExtentUU = 0.5f * float(VolumeDim > 0 ? VolumeDim : VoxelGIVolume::GetDim())
	                         * float(VoxelLF::CellSizeUU);
	//
	// The clamp SLIDES the band rather than truncating it. Clamping FadeEnd alone
	// would leave FadeStart where it was and collapse a 1600 UU fade into a
	// 40 UU one at VolumeDim 192 -- which is the hard ring this is meant to
	// prevent, arrived at from the other direction. Preserving the band width and
	// moving both ends down degrades to "GI fades out somewhat early", which is
	// the failure mode a player cannot see.
	const float MaxFadeEndUU = FMath::Max(2.0f * float(VoxelLF::BrickEdgeUU),
	                                      HalfExtentUU - float(VoxelLF::BrickEdgeUU));
	const float WantEndUU = VoxelGI::GetFadeEndUU();
	const float WantStartUU = FMath::Min(VoxelGI::GetFadeStartUU(), WantEndUU - float(VoxelLF::CellSizeUU));
	const float ShiftUU = FMath::Max(0.0f, WantEndUU - MaxFadeEndUU);
	New.FadeEndUU = WantEndUU - ShiftUU;
	New.FadeStartUU = FMath::Max(float(VoxelLF::BrickEdgeUU), WantStartUU - ShiftUU);
	New.FadeStartUU = FMath::Min(New.FadeStartUU, New.FadeEndUU - float(VoxelLF::CellSizeUU));

	if (bVolumeSettingsValid && New == LastVolumeSettings)
	{
		return;
	}
	// Announce the SHADING terms whenever they move, not the origin/fade-centre
	// (which move every frame the camera does and would drown the log). This is
	// the line that makes a fired clamp visible: risk 8's failure mode is a
	// plausible image, so "the fade you asked for is not the fade you got" has to
	// be greppable rather than inferable.
	if (!bVolumeSettingsValid
	    || New.bEnabled != LastVolumeSettings.bEnabled
	    || New.bLocalEnabled != LastVolumeSettings.bLocalEnabled
	    || New.Strength != LastVolumeSettings.Strength
	    || New.AmbientFloor != LastVolumeSettings.AmbientFloor
	    || New.FadeStartUU != LastVolumeSettings.FadeStartUU
	    || New.FadeEndUU != LastVolumeSettings.FadeEndUU
	    || New.DebugVis != LastVolumeSettings.DebugVis)
	{
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VoxelGI volume params: enabled=%d localEnabled=%d strength=%.3f ambientFloor=%.3f ")
		       TEXT("fade=%.0f..%.0f UU (cvars asked %.0f..%.0f; volume half-extent %.0f) debugVis=%d"),
		       New.bEnabled ? 1 : 0, New.bLocalEnabled ? 1 : 0, New.Strength, New.AmbientFloor,
		       New.FadeStartUU, New.FadeEndUU,
		       VoxelGI::GetFadeStartUU(), VoxelGI::GetFadeEndUU(), HalfExtentUU, New.DebugVis);
	}
	LastVolumeSettings = New;
	bVolumeSettingsValid = true;

	ENQUEUE_RENDER_COMMAND(VoxelGIVolumeParams)(
		[New](FRHICommandListImmediate& RHICmdList)
	{
		GVoxelGIVolume.UpdateParameters_RenderThread(New);
	});
}

void UVoxelGISubsystem::TickVolume()
{
	if (!VoxelGIVolume::IsEnabled())
	{
		// One publish on the way down, so a live toggle-off actually reaches the
		// shader instead of leaving the last Enabled=1 buffer bound forever.
		if (bVolumeSettingsValid && LastVolumeSettings.bEnabled)
		{
			FVoxelGIVolumeSettings Off = LastVolumeSettings;
			Off.bEnabled = false;
			LastVolumeSettings = Off;
			ENQUEUE_RENDER_COMMAND(VoxelGIVolumeOff)(
				[Off](FRHICommandListImmediate&) { GVoxelGIVolume.UpdateParameters_RenderThread(Off); });
		}
		return;
	}
	if (!EnsureVolumeOrigin())
	{
		return;
	}
	if (!bVolumeAllocated)
	{
		bVolumeAllocated = true;
		// bWantLocal is read HERE, on the game thread, and carried into the command
		// -- voxel.GI.LocalLights is ECVF_RenderThreadSafe and a read on the render
		// thread returns a shadow value that lags by up to a frame. See the note at
		// VoxelGIVolume.cpp's VolumeLocal allocation.
		const bool bWantLocal = VoxelGIVolume::IsLocalEnabled();
		ENQUEUE_RENDER_COMMAND(VoxelGIVolumeAlloc)(
			[bWantLocal](FRHICommandListImmediate& RHICmdList)
			{ GVoxelGIVolume.EnsureAllocated_RenderThread(RHICmdList, bWantLocal); });
	}

	// --- local lights: arm, allocate, splat ---------------------------------
	//
	// READ PER FRAME, not latched. voxel.GI.Volume's own "read per frame" claim was
	// false until Wave B because UpdateParameters_RenderThread had three callers
	// and none of them ran per frame; this switch is wired through the same tick so
	// it does not repeat that.
	const bool bLocalWanted = VoxelGIVolume::IsLocalEnabled();
	if (bLocalWanted != bLocalLightsActive)
	{
		bLocalLightsActive = bLocalWanted;
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VoxelGI local lights: voxel.GI.LocalLights=%d (localDim=%d, texel %.0f UU, ")
		       TEXT("occlusion=%d, maxUploadsPerFrame=%d, %d light(s) registered)"),
		       bLocalWanted ? 1 : 0, VolumeLocalDim, LocalTexelUU(),
		       CVarGILocalLightOcclusion.GetValueOnGameThread(),
		       CVarGIMaxLocalUploadsPerFrame.GetValueOnGameThread(), LocalLights.Num());
		if (bLocalWanted)
		{
			// Arriving late is the normal case (the switch is off at startup), so
			// everything the first-time path in EnsureVolumeOrigin would have done
			// happens here instead: the mirror, the texture, and a full re-splat.
			if (VolumeLocalDim <= 0)
			{
				VolumeLocalDim = VoxelGIVolume::GetLocalDim();
			}
			if (VolumeLocalShadow.Num() == 0 && VolumeLocalDim > 0)
			{
				VolumeLocalShadow.SetNumZeroed(int64(VolumeLocalDim) * VolumeLocalDim * VolumeLocalDim * 4);
			}
			bLocalVolumeAllocated = false;
			MarkLocalVolumeDirty();
		}
	}
	if (bLocalWanted && !bLocalVolumeAllocated)
	{
		bLocalVolumeAllocated = true;
		// Re-enters EnsureAllocated_RenderThread, which guards each texture
		// separately: the irradiance volumes already exist and are left alone.
		//
		// bWantLocal is passed as a hard TRUE rather than letting the callee re-read
		// the cvar. bLocalWanted was read on THIS thread and this branch latches
		// bLocalVolumeAllocated, so a callee that re-read voxel.GI.LocalLights on the
		// render thread and got the pre-sink shadow value of 0 would skip the
		// allocation and never be asked again -- the feature would then be on
		// everywhere except in the one place that matters.
		ENQUEUE_RENDER_COMMAND(VoxelGIVolumeLocalAlloc)(
			[](FRHICommandListImmediate& RHICmdList)
			{ GVoxelGIVolume.EnsureAllocated_RenderThread(RHICmdList, /*bWantLocal*/ true); });
	}

	if (bVolumeRecentring)
	{
		// The per-brick drain is suspended for the duration: every resident brick
		// is being re-encoded anyway, and letting the two upload paths interleave
		// would make "the origin uniform changed on exactly one frame" harder to
		// state than it needs to be.
		StepVolumeRecentre();
	}
	else if (VolumeNeedsRecentre())
	{
		BeginVolumeRecentre();
		StepVolumeRecentre();
	}
	else
	{
		// The accessor rather than Tick()'s bMarchArm: this is TickVolume(), a
		// different function, and the local does not reach here. On the march
		// arm there is nothing to drain anyway (nothing pushes the queue), so
		// this is belt-and-braces -- but a queue that somehow filled would
		// otherwise upload CPU-encoded texels over the GPU's own writes, which
		// renders as lighting that flickers between two answers.
		DrainVolumeUploads(VoxelGI::IsMarchBricksEnabled()
			? 0 : FMath::Max(0, CVarGIMaxBrickUploadsPerFrame.GetValueOnGameThread()));
		// Suspended for the duration of a re-centre, exactly as the per-brick drain
		// is: during one, every local texel is being restaged by
		// RestageVolumeZRange's own call anyway, and letting the two paths
		// interleave would make "the origin uniform changed on exactly one frame"
		// harder to state than it needs to be.
		DrainLocalSplats(FMath::Max(0, CVarGIMaxLocalUploadsPerFrame.GetValueOnGameThread()));
	}

	PushVolumeParamsIfChanged();
}

int32 UVoxelGISubsystem::DrainVolumeUploads(int32 Budget)
{
	// TIMED HERE, NOT AT THE TWO CALL SITES, so every early-out is covered and
	// the ms and the count can never be drawn from different sets of
	// invocations. Both call sites (TickVolume's budgeted drain and
	// FlushVolume's unbudgeted one) land in the same pair of accumulators.
	//
	// StatUploadBricks counts BRICKS ENCODED AND STAGED -- the same denominator
	// the ms covers. A frame that drains nothing adds 0 to both, so the printed
	// mean is over the work that happened and never over frames that did none.
	const double DrainStartSeconds = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		StatUploadMs += (FPlatformTime::Seconds() - DrainStartSeconds) * 1000.0;
	};

	if (!Field || !VoxelGIVolume::IsEnabled())
	{
		return 0;
	}
	if (VolumeUploadQueue.Num() == 0 || !EnsureVolumeOrigin())
	{
		return 0;
	}

	// 1) Pop the frame's work list. A brick outside the volume's extent is
	//    dropped here rather than left in the queue: the volume covers
	//    +/-Dim*20 UU and the field's build radius is larger, so most of the
	//    field is legitimately off-volume and re-queuing it forever would turn
	//    the queue into a treadmill.
	TArray<FIntVector> Batch;
	TArray<FIntVector> TexelBase; // per Batch entry, texel coords of the brick's (0,0,0) cell
	while ((Budget < 0 || Batch.Num() < Budget) && VolumeUploadQueue.Num() > 0)
	{
		const FIntVector Key = VolumeUploadQueue[0];
		VolumeUploadQueue.RemoveAt(0, 1, EAllowShrinking::No);
		VolumeUploadSet.Remove(Key);

		const FIntVector Base(Key.X * VoxelLF::BrickEdgeCells - VolumeCellOrigin.X,
		                      Key.Y * VoxelLF::BrickEdgeCells - VolumeCellOrigin.Y,
		                      Key.Z * VoxelLF::BrickEdgeCells - VolumeCellOrigin.Z);
		const int32 E = VoxelLF::BrickEdgeCells;
		if (Base.X < 0 || Base.Y < 0 || Base.Z < 0 ||
		    Base.X + E > VolumeDim || Base.Y + E > VolumeDim || Base.Z + E > VolumeDim)
		{
			continue;
		}
		Batch.Add(Key);
		TexelBase.Add(Base);
	}
	if (Batch.Num() == 0)
	{
		return 0;
	}

	// 2) Encode into the CPU shadow, under ONE read scope, on the GAME thread.
	//    §1: anything that reads the field to build texel data must be here, not
	//    on the render thread.
	{
		const FVoxelLightField::FReadScope Read(*Field);
		uint8 BrickPos[FVoxelLightField::BrickTexelBytes];
		uint8 BrickNeg[FVoxelLightField::BrickTexelBytes];
		for (int32 I = 0; I < Batch.Num(); ++I)
		{
			// Returns false (and all-zero bytes) for a brick that is absent
			// (evicted) or re-voxelized-but-not-yet-solved. Both of those MUST
			// reach the volume, or a dug tunnel keeps its pre-dig lighting --
			// which is exactly why this path does not skip on false.
			Read.EncodeBrick(Batch[I], BrickPos, BrickNeg);

			const FIntVector& Base = TexelBase[I];
			for (int32 Z = 0; Z < VoxelLF::BrickEdgeCells; ++Z)
			for (int32 Y = 0; Y < VoxelLF::BrickEdgeCells; ++Y)
			{
				const int64 DstOffset = ((int64(Base.Z + Z) * VolumeDim + (Base.Y + Y)) * VolumeDim + Base.X) * 4;
				const int32 SrcOffset = (Z * VoxelLF::BrickEdgeCells + Y) * VoxelLF::BrickEdgeCells * 4;
				FMemory::Memcpy(VolumeShadow.GetData() + DstOffset, BrickPos + SrcOffset,
				                VoxelLF::BrickEdgeCells * 4);
				FMemory::Memcpy(VolumeShadowNeg.GetData() + DstOffset, BrickNeg + SrcOffset,
				                VoxelLF::BrickEdgeCells * 4);
			}
		}
	}

	// 3) Merge into X-runs. THE reason this exists: the D3D12 staging row pitch
	//    is Align(Width*4, 256), so an 8-texel-wide brick stages 16 KB to move
	//    2 KB. Grouping by (texelY, texelZ) and coalescing along X turns a dig's
	//    125 per-brick calls into 25 wide ones with no waste.
	TArray<int32> Order;
	Order.SetNumUninitialized(TexelBase.Num());
	for (int32 I = 0; I < Order.Num(); ++I) { Order[I] = I; }
	Order.Sort([&TexelBase](int32 A, int32 B)
	{
		const FIntVector& VA = TexelBase[A];
		const FIntVector& VB = TexelBase[B];
		if (VA.Z != VB.Z) { return VA.Z < VB.Z; }
		if (VA.Y != VB.Y) { return VA.Y < VB.Y; }
		return VA.X < VB.X;
	});

	TArray<FVoxelGIVolumeRun> Runs;
	const int32 E = VoxelLF::BrickEdgeCells;
	int32 I = 0;
	while (I < Order.Num())
	{
		const FIntVector& Start = TexelBase[Order[I]];
		int32 EndX = Start.X + E; // exclusive
		int32 J = I + 1;
		while (J < Order.Num())
		{
			const FIntVector& Next = TexelBase[Order[J]];
			if (Next.Y != Start.Y || Next.Z != Start.Z)
			{
				break;
			}
			if (Next.X > EndX + kVolumeRunGapBricks * E)
			{
				break;
			}
			EndX = FMath::Max(EndX, Next.X + E);
			++J;
		}

		FVoxelGIVolumeRun& Run = Runs.AddDefaulted_GetRef();
		Run.Min = FIntVector(Start.X, Start.Y, Start.Z);
		Run.Size = FIntVector(EndX - Start.X, E, E);
		Run.TexelsPos.SetNumUninitialized(int64(Run.Size.X) * Run.Size.Y * Run.Size.Z * 4);
		Run.TexelsNeg.SetNumUninitialized(int64(Run.Size.X) * Run.Size.Y * Run.Size.Z * 4);
		// Compact the run out of the shadow. The shadow is the source of truth
		// for what is on the GPU, so a run always carries the LATEST bytes for
		// every brick it spans -- including bricks that were not in this frame's
		// batch but happen to sit inside a coalesced gap.
		for (int32 Z = 0; Z < Run.Size.Z; ++Z)
		for (int32 Y = 0; Y < Run.Size.Y; ++Y)
		{
			const int64 Src = ((int64(Run.Min.Z + Z) * VolumeDim + (Run.Min.Y + Y)) * VolumeDim + Run.Min.X) * 4;
			const int64 Dst = (int64(Z) * Run.Size.Y + Y) * Run.Size.X * 4;
			FMemory::Memcpy(Run.TexelsPos.GetData() + Dst, VolumeShadow.GetData() + Src,
			                int64(Run.Size.X) * 4);
			FMemory::Memcpy(Run.TexelsNeg.GetData() + Dst, VolumeShadowNeg.GetData() + Src,
			                int64(Run.Size.X) * 4);
		}
		I = J;
	}

	VolumeBricksUploaded += Batch.Num();
	VolumeUploadedThisFrame += Batch.Num();
	StatUploadBricks += Batch.Num();
	VolumeRunsUploaded += Runs.Num();
	if (FirstVolumeUploadSeconds == 0.0)
	{
		FirstVolumeUploadSeconds = FPlatformTime::Seconds();
	}

	// 4) One render command for the whole frame's runs -- BeginUpdateTexture3D
	//    asserts IsInParallelRenderingThread(), and the payload is owned by the
	//    lambda so nothing on the game thread can mutate it mid-upload.
	ENQUEUE_RENDER_COMMAND(VoxelGIVolumeUpload)(
		[Payload = MoveTemp(Runs)](FRHICommandListImmediate& RHICmdList)
	{
		for (const FVoxelGIVolumeRun& Run : Payload)
		{
			GVoxelGIVolume.UpdateTexels_RenderThread(RHICmdList, Run.Min, Run.Size,
			                                         Run.TexelsPos.GetData(), Run.TexelsNeg.GetData());
		}
	});

	return Batch.Num();
}

// --- local lights (docs/sky-and-local-light-plan.md §2.2, phase L1) ---------
//
// WHAT THIS IS AND WHAT IT IS NOT. There is no additive path from a light to a
// pixel today: M_VoxelTerrain computes BaseColor = albedo * VertexColor.G *
// DebugTint, the pooled vertex factory can only SCALE .g, and a deferred point
// light's contribution is proportional to BaseColor -- so in a cave, where
// G ~= AO * lerp(0.06, 1, ~0) ~= 0.06, a torch lights a wall at 6% of its
// albedo. L1 fixes exactly that 6% and nothing else: it splats an UN-CRUSH
// SCALAR into VolumeLocal's A channel, the factory takes Ambient =
// max(Ambient, A), and the deferred light then has a real BaseColor to
// illuminate. The A channel emits nothing. RGB stays zero; L2 owns it.
//
// ON THE GAME THREAD, which is idle ~75% of the frame (docs/backlog.md §0.1).
// The render thread is the bound one, so the cheap side is where this belongs,
// and everything here is game-thread work that ends in one UpdateTexture3D.

double UVoxelGISubsystem::LocalTexelUU() const
{
	// The two volumes cover the SAME BOX -- that identity is what lets the factory
	// reuse Interpolants.GIUVW with zero new interpolants -- so the local texel
	// size follows from the two dims and is never a second hardcoded constant.
	const int32 MainDim = VolumeDim > 0 ? VolumeDim : VoxelGIVolume::GetDim();
	const int32 LocalDim = VolumeLocalDim > 0 ? VolumeLocalDim : VoxelGIVolume::GetLocalDim();
	return double(MainDim) * double(VoxelLF::CellSizeUU) / double(FMath::Max(1, LocalDim));
}

float UVoxelGISubsystem::LocalLightContributionAt(const FVoxelLocalLight& Light, const FVector& PointUU,
                                                 const FVoxelLightField::FReadScope& Read) const
{
	const double R = FMath::Max(1.0, Light.RadiusUU);
	const double D = FVector::Dist(Light.PositionUU, PointUU);
	if (D >= R)
	{
		return 0.f;
	}

	// INVERSE-SQUARE, with a CORE and a WINDOW, and both of those earn their place.
	//
	// The core (a quarter of the reach) exists because A is not a radiance: it is
	// "how lit is this cell by local sources, 0..1", and 1/d^2 without a core
	// diverges at the light and would make the un-crush a function of how close
	// the torch happens to be rather than of whether it reaches. Inside the core
	// the surface is simply un-crushed, which is the state a lit surface is
	// supposed to be in.
	//
	// The window is UE's own (1-(d/R)^4)^2 shape and it takes the term to EXACTLY
	// zero at the radius. Without it the influence sphere ends on a step, and
	// because the splat's dirty box is derived from the same radius, that step
	// would land on the boundary between written and unwritten texels -- a hard
	// edge that reads as an addressing bug.
	const double CoreUU = 0.25 * R;
	const double Inv = (D <= CoreUU) ? 1.0 : (CoreUU * CoreUU) / FMath::Max(1.0, D * D);
	const double Ratio = D / R;
	const double Window = FMath::Square(1.0 - Ratio * Ratio * Ratio * Ratio);
	float A = float(double(Light.Intensity) * Inv * Window);
	if (A <= 0.f)
	{
		return 0.f;
	}

	// The occlusion march, which is what makes a wall a wall. Skippable via
	// voxel.GI.LocalLightOcclusion 0 so the march's own contribution is an A/B
	// from one build rather than an assertion.
	if (CVarGILocalLightOcclusion.GetValueOnGameThread() != 0)
	{
		A *= Read.LocalTransmittance(Light.PositionUU, PointUU);
	}
	return FMath::Clamp(A, 0.f, 1.f);
}

float UVoxelGISubsystem::LocalUnCrushAt(const FVector& PointUU, const FVoxelLightField::FReadScope& Read) const
{
	// MAX over all local sources, per the channel contract. Not a sum: A is a
	// visibility-like scalar bounded by 1, and summing two torches would push the
	// un-crush past the unoccluded 1.0 that docs/lighting-weather-plan.md §2.3
	// fixes for all time.
	float Best = 0.f;
	for (const FVoxelLocalLight& Light : LocalLights)
	{
		Best = FMath::Max(Best, LocalLightContributionAt(Light, PointUU, Read));
		if (Best >= 1.f)
		{
			break;
		}
	}
	return Best;
}

int32 UVoxelGISubsystem::AddLocalLight(const FVector& PositionUU, double RadiusUU, float Intensity)
{
	if (!bVolumeOriginSet)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VoxelGI local light: refused -- the volume origin is not established yet (needs ")
		       TEXT("voxel.GI.Enabled 1, voxel.GI.Volume 1, voxel.Stream.GPU 1 and one tick with a pool)."));
		return 0;
	}
	FVoxelLocalLight& Light = LocalLights.AddDefaulted_GetRef();
	Light.PositionUU = PositionUU;
	Light.RadiusUU = FMath::Clamp(RadiusUU, double(VoxelLF::CellSizeUU), 20000.0);
	Light.Intensity = FMath::Clamp(Intensity, 0.f, 1.f);
	Light.Id = NextLocalLightId++;
	MarkLocalLightDirty(Light.PositionUU, Light.RadiusUU);
	return Light.Id;
}

bool UVoxelGISubsystem::MoveLocalLight(int32 LightId, const FVector& PositionUU)
{
	for (FVoxelLocalLight& Light : LocalLights)
	{
		if (Light.Id != LightId)
		{
			continue;
		}
		// BOTH neighbourhoods, old first. Every dirty box is recomputed from the
		// whole light list, so marking the old one is what removes the lit ghost the
		// light leaves behind; skipping it looks like a falloff bug rather than a
		// missing clear.
		MarkLocalLightDirty(Light.PositionUU, Light.RadiusUU);
		Light.PositionUU = PositionUU;
		MarkLocalLightDirty(Light.PositionUU, Light.RadiusUU);
		return true;
	}
	return false;
}

bool UVoxelGISubsystem::RemoveLocalLight(int32 LightId)
{
	for (int32 I = 0; I < LocalLights.Num(); ++I)
	{
		if (LocalLights[I].Id != LightId)
		{
			continue;
		}
		const FVector WasAt = LocalLights[I].PositionUU;
		const double WasR = LocalLights[I].RadiusUU;
		if (AActor* Actor = LocalLights[I].TestActor.Get())
		{
			Actor->Destroy();
		}
		LocalLights.RemoveAt(I);
		// Marked AFTER the removal, so the box is recomputed from a list that no
		// longer contains this light.
		MarkLocalLightDirty(WasAt, WasR);
		return true;
	}
	return false;
}

void UVoxelGISubsystem::ClearLocalLights()
{
	const int32 Num = LocalLights.Num();
	for (FVoxelLocalLight& Light : LocalLights)
	{
		if (AActor* Actor = Light.TestActor.Get())
		{
			Actor->Destroy();
		}
	}
	LocalLights.Reset();
	// The whole volume, not the union of the boxes just dropped: this is the
	// torch-off arm of the L1 gate and "some texel kept its light" is exactly the
	// failure that would make the arm read as a pass.
	MarkLocalVolumeDirty();
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VoxelGI local lights: cleared %d light(s) and their deferred actors; VolumeLocal queued for ")
	       TEXT("a full re-splat (%d boxes)"),
	       Num, LocalSplatQueue.Num());
}

void UVoxelGISubsystem::MarkLocalLightDirty(const FVector& PositionUU, double RadiusUU)
{
	if (VolumeLocalDim <= 0 || VolumeLocalShadow.Num() == 0)
	{
		return;
	}
	const double T = LocalTexelUU();
	// Texel i's CENTRE is VolumeOriginWorldUU + (i+0.5)*T, so a world coordinate's
	// texel index is (W - Origin)/T - 0.5. Same convention as the irradiance
	// volumes' P/40 - 0.5, which is what makes one brick-snapped origin serve both.
	//
	// ONE TEXEL OF MARGIN on each side. The falloff window reaches exactly 0 at
	// the radius, but the volume is read with hardware trilinear, so the first ring
	// of texels OUTSIDE the light still contributes to samples inside it and must
	// therefore be written (as 0) rather than left holding whatever was there.
	auto LoIdx = [T](double W, double O, double R) { return int32(FMath::FloorToDouble((W - R - O) / T - 0.5)) - 1; };
	auto HiIdx = [T](double W, double O, double R) { return int32(FMath::CeilToDouble((W + R - O) / T - 0.5)) + 1; };
	const FIntVector RawMin(LoIdx(PositionUU.X, VolumeOriginWorldUU.X, RadiusUU),
	                        LoIdx(PositionUU.Y, VolumeOriginWorldUU.Y, RadiusUU),
	                        LoIdx(PositionUU.Z, VolumeOriginWorldUU.Z, RadiusUU));
	const FIntVector RawMax(HiIdx(PositionUU.X, VolumeOriginWorldUU.X, RadiusUU),
	                        HiIdx(PositionUU.Y, VolumeOriginWorldUU.Y, RadiusUU),
	                        HiIdx(PositionUU.Z, VolumeOriginWorldUU.Z, RadiusUU));
	const FIntVector Min(FMath::Clamp(RawMin.X, 0, VolumeLocalDim - 1),
	                     FMath::Clamp(RawMin.Y, 0, VolumeLocalDim - 1),
	                     FMath::Clamp(RawMin.Z, 0, VolumeLocalDim - 1));
	const FIntVector Max(FMath::Clamp(RawMax.X, 0, VolumeLocalDim - 1),
	                     FMath::Clamp(RawMax.Y, 0, VolumeLocalDim - 1),
	                     FMath::Clamp(RawMax.Z, 0, VolumeLocalDim - 1));
	if (RawMax.X < 0 || RawMax.Y < 0 || RawMax.Z < 0 ||
	    RawMin.X >= VolumeLocalDim || RawMin.Y >= VolumeLocalDim || RawMin.Z >= VolumeLocalDim)
	{
		return; // entirely outside the volume: nothing to splat, and not an error
	}

	// SLICED IN Z, exactly as MarkLocalVolumeDirty is, and for the same reason:
	// otherwise voxel.GI.MaxLocalUploadsPerFrame does not bound anything for a
	// SINGLE light and its help text is a lie.
	//
	// The arithmetic that forces this. The plan (§2.2) estimates a 10 m light at
	// "~12^3 texels ~= 7 KB", which is the figure for a 160 UU texel. At the
	// shipped voxel.GI.LocalVolumeDim 96 against VolumeDim 192 the texel is 80 UU,
	// so a 1000 UU REACH spans 2000/80 = 25 texels per axis, +1 margin each side =
	// 27 -- 27^3 = 19,683 texels and 79 KB, eleven times the estimate. The texels
	// are cheap; the OCCLUSION MARCHES are not, at up to 25 opacity taps each, so
	// one box would put ~0.5M taps in the single frame the torch was placed in.
	// Slicing turns that into ~4 boxes the budget can spread, and the splat is
	// idempotent per box (it recomputes from the whole light list), so the slices
	// are independent rather than a partially applied whole.
	constexpr int32 kSlabTexels = 8;
	constexpr int32 kMaxLocalSplatBoxes = 64;
	for (int32 Z = Min.Z; Z <= Max.Z; Z += kSlabTexels)
	{
		const int32 SizeZ = FMath::Min(kSlabTexels, Max.Z - Z + 1);
		const FVoxelLocalSplatBox Box{FIntVector(Min.X, Min.Y, Z),
		                              FIntVector(Max.X - Min.X + 1, Max.Y - Min.Y + 1, SizeZ)};
		bool bAlready = false;
		for (const FVoxelLocalSplatBox& Existing : LocalSplatQueue)
		{
			if (Existing.Min == Box.Min && Existing.Size == Box.Size)
			{
				// A light marked twice in one frame is one slice of work. Matched on
				// the WHOLE box, not just Min: a slice and the volume-wide slab that
				// starts at the same texel are different amounts of work and
				// collapsing them would drop the wider one.
				bAlready = true;
				break;
			}
		}
		if (bAlready)
		{
			continue;
		}
		// OVERFLOW COLLAPSES TO A FULL RESTAGE rather than dropping slices. Dropping
		// one drops a CLEAR, and a lost clear is a permanent lit ghost -- the queue's
		// contents are not interchangeable the way the voxelize queue's are.
		if (LocalSplatQueue.Num() >= kMaxLocalSplatBoxes)
		{
			MarkLocalVolumeDirty();
			return;
		}
		LocalSplatQueue.Add(Box);
	}
}

void UVoxelGISubsystem::MarkLocalVolumeDirty()
{
	if (VolumeLocalDim <= 0 || VolumeLocalShadow.Num() == 0)
	{
		return;
	}
	// Z SLABS, NOT ONE BOX. A single whole-volume box would be one entry against a
	// per-frame budget of 16, i.e. the budget would stop bounding anything and a
	// re-arm would cost a whole volume of work in one frame.
	LocalSplatQueue.Reset();
	constexpr int32 kSlabTexels = 8;
	for (int32 Z = 0; Z < VolumeLocalDim; Z += kSlabTexels)
	{
		const int32 SizeZ = FMath::Min(kSlabTexels, VolumeLocalDim - Z);
		LocalSplatQueue.Add(FVoxelLocalSplatBox{FIntVector(0, 0, Z),
		                                        FIntVector(VolumeLocalDim, VolumeLocalDim, SizeZ)});
	}
}

void UVoxelGISubsystem::MarkLocalLightsDirtyForBrick(const FIntVector& BrickCoord)
{
	if (LocalLights.Num() == 0 || VolumeLocalShadow.Num() == 0)
	{
		return;
	}
	// The brick's world box, against each light's reach. A dig, a stream-in or an
	// eviction changes what the occlusion march sees, so any light that could see
	// this brick has a stale splat -- this is what makes torchlight follow a tunnel
	// as it is dug, and it is also what eventually corrects the
	// "not resident = not an occluder" assumption the march makes.
	const FVector BrickMin(double(BrickCoord.X) * VoxelLF::BrickEdgeUU,
	                       double(BrickCoord.Y) * VoxelLF::BrickEdgeUU,
	                       double(BrickCoord.Z) * VoxelLF::BrickEdgeUU);
	const FVector BrickMax = BrickMin + FVector(double(VoxelLF::BrickEdgeUU));
	for (const FVoxelLocalLight& Light : LocalLights)
	{
		const FVector Closest(FMath::Clamp(Light.PositionUU.X, BrickMin.X, BrickMax.X),
		                      FMath::Clamp(Light.PositionUU.Y, BrickMin.Y, BrickMax.Y),
		                      FMath::Clamp(Light.PositionUU.Z, BrickMin.Z, BrickMax.Z));
		if (FVector::Dist(Closest, Light.PositionUU) <= Light.RadiusUU)
		{
			MarkLocalLightDirty(Light.PositionUU, Light.RadiusUU);
		}
	}
}

void UVoxelGISubsystem::SplatLocalBox(const FIntVector& Min, const FIntVector& Size)
{
	if (!Field || VolumeLocalDim <= 0 || VolumeLocalShadow.Num() == 0)
	{
		return;
	}
	if (Size.X <= 0 || Size.Y <= 0 || Size.Z <= 0)
	{
		return;
	}
	if (Min.X < 0 || Min.Y < 0 || Min.Z < 0 ||
	    Min.X + Size.X > VolumeLocalDim || Min.Y + Size.Y > VolumeLocalDim || Min.Z + Size.Z > VolumeLocalDim)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VoxelGI local splat: rejected out-of-range box min=(%d,%d,%d) size=(%d,%d,%d) localDim=%d"),
		       Min.X, Min.Y, Min.Z, Size.X, Size.Y, Size.Z, VolumeLocalDim);
		return;
	}

	const int32 Dim = VolumeLocalDim;
	const double T = LocalTexelUU();
	const double StartSeconds = FPlatformTime::Seconds();

	// 1) Zero the box. A texel no light reaches MUST read A=0 -- "no local
	//    source", which the factory's max(Ambient, A) turns into no change at all.
	//    All four bytes go, not just A: RGB is premultiplied-by-validity territory
	//    in the sibling volumes and L2's tail radiance here, and leaving stale RGB
	//    under a zero A is exactly the "energy from nowhere" the encoder's own
	//    comment refuses.
	for (int32 Z = 0; Z < Size.Z; ++Z)
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		const int64 Offset = ((int64(Min.Z + Z) * Dim + (Min.Y + Y)) * Dim + Min.X) * 4;
		FMemory::Memzero(VolumeLocalShadow.GetData() + Offset, int64(Size.X) * 4);
	}

	// 2) Max in every light that overlaps the box, iterating each light's OWN
	//    intersection rather than the whole box per light: the marches are the
	//    cost here, and a full restage slab is mostly empty space no light reaches.
	if (LocalLights.Num() > 0)
	{
		const FVoxelLightField::FReadScope Read(*Field);
		for (const FVoxelLocalLight& Light : LocalLights)
		{
			// Texel index range of this light, clipped to the box being splatted.
			auto LoIdx = [T](double W, double O, double R) { return int32(FMath::FloorToDouble((W - R - O) / T - 0.5)); };
			auto HiIdx = [T](double W, double O, double R) { return int32(FMath::CeilToDouble((W + R - O) / T - 0.5)); };
			const int32 X0 = FMath::Max(Min.X, LoIdx(Light.PositionUU.X, VolumeOriginWorldUU.X, Light.RadiusUU));
			const int32 Y0 = FMath::Max(Min.Y, LoIdx(Light.PositionUU.Y, VolumeOriginWorldUU.Y, Light.RadiusUU));
			const int32 Z0 = FMath::Max(Min.Z, LoIdx(Light.PositionUU.Z, VolumeOriginWorldUU.Z, Light.RadiusUU));
			const int32 X1 = FMath::Min(Min.X + Size.X - 1, HiIdx(Light.PositionUU.X, VolumeOriginWorldUU.X, Light.RadiusUU));
			const int32 Y1 = FMath::Min(Min.Y + Size.Y - 1, HiIdx(Light.PositionUU.Y, VolumeOriginWorldUU.Y, Light.RadiusUU));
			const int32 Z1 = FMath::Min(Min.Z + Size.Z - 1, HiIdx(Light.PositionUU.Z, VolumeOriginWorldUU.Z, Light.RadiusUU));

			for (int32 Z = Z0; Z <= Z1; ++Z)
			for (int32 Y = Y0; Y <= Y1; ++Y)
			for (int32 X = X0; X <= X1; ++X)
			{
				const FVector P(VolumeOriginWorldUU.X + (double(X) + 0.5) * T,
				                VolumeOriginWorldUU.Y + (double(Y) + 0.5) * T,
				                VolumeOriginWorldUU.Z + (double(Z) + 0.5) * T);
				const float A = LocalLightContributionAt(Light, P, Read);
				if (A <= 0.f)
				{
					continue;
				}
				const uint8 Byte = uint8(FMath::Clamp(FMath::RoundToInt(A * 255.f), 0, 255));
				uint8& Dst = VolumeLocalShadow[((int64(Z) * Dim + Y) * Dim + X) * 4 + 3];
				Dst = FMath::Max(Dst, Byte);
				++LocalTexelsLit;
			}
		}
	}

	// 3) Compact the box out of the mirror and upload it. The mirror is the source
	//    of truth for what is on the GPU, so the payload always carries the LATEST
	//    bytes -- including contributions from a light that was not the reason this
	//    box became dirty.
	TArray<uint8> Payload;
	Payload.SetNumUninitialized(int64(Size.X) * Size.Y * Size.Z * 4);
	for (int32 Z = 0; Z < Size.Z; ++Z)
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		const int64 Src = ((int64(Min.Z + Z) * Dim + (Min.Y + Y)) * Dim + Min.X) * 4;
		const int64 Dst = (int64(Z) * Size.Y + Y) * int64(Size.X) * 4;
		FMemory::Memcpy(Payload.GetData() + Dst, VolumeLocalShadow.GetData() + Src, int64(Size.X) * 4);
	}

	ENQUEUE_RENDER_COMMAND(VoxelGIVolumeLocalUpload)(
		[Min, Size, Bytes = MoveTemp(Payload)](FRHICommandListImmediate& RHICmdList)
	{
		GVoxelGIVolume.UpdateLocalTexels_RenderThread(RHICmdList, Min, Size, Bytes.GetData());
	});

	++LocalBoxesSplatted;
	LocalSplatMs += (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
}

int32 UVoxelGISubsystem::DrainLocalSplats(int32 Budget)
{
	if (LocalSplatQueue.Num() == 0 || !VoxelGIVolume::IsLocalEnabled())
	{
		return 0;
	}
	int32 Done = 0;
	while ((Budget < 0 || Done < Budget) && LocalSplatQueue.Num() > 0)
	{
		const FVoxelLocalSplatBox Box = LocalSplatQueue[0];
		LocalSplatQueue.RemoveAt(0, 1, EAllowShrinking::No);
		SplatLocalBox(Box.Min, Box.Size);
		++Done;
	}
	return Done;
}

void UVoxelGISubsystem::RestageLocalZRange(int32 Z0, int32 Z1)
{
	if (VolumeLocalDim <= 0 || VolumeLocalShadow.Num() == 0 || VolumeDim <= 0 || Z1 <= Z0)
	{
		return;
	}
	// MAIN texel rows -> LOCAL texel rows. Floor the low end and ceil the high one
	// so the mapped ranges COVER the volume with no gap even when the two dims are
	// not integer multiples; double-covering a boundary texel is harmless because
	// SplatLocalBox recomputes from the whole light list rather than accumulating.
	const double Scale = double(VolumeLocalDim) / double(VolumeDim);
	const int32 LocalZ0 = FMath::Clamp(int32(FMath::FloorToDouble(double(Z0) * Scale)), 0, VolumeLocalDim - 1);
	const int32 LocalZ1 = FMath::Clamp(int32(FMath::CeilToDouble(double(Z1) * Scale)), LocalZ0 + 1, VolumeLocalDim);
	SplatLocalBox(FIntVector(0, 0, LocalZ0), FIntVector(VolumeLocalDim, VolumeLocalDim, LocalZ1 - LocalZ0));
}

// --- voxel.GI.VolumeCheck ---------------------------------------------------
//
// THE DELIVERABLE FOR STEP 2. "The volume matches the field" is otherwise only
// checkable by looking at two screenshots of a cave, which is slow, subjective,
// and cannot distinguish a half-cell addressing shift from a fade retune. This
// turns it into a grep.
//
// Method: pick random SOLVED cells, jitter the probe point off the cell centre
// (a cell centre lands exactly on a texel and would make trilinear degenerate
// to a single tap -- a test that cannot fail), then compute both sides at the
// SAME point:
//
//   * the shader side, evaluated in software from VolumeShadow -- the actual
//     staged bytes, so an addressing or run-merge bug shows up here and a
//     re-encode would have hidden it;
//   * FVoxelLightField::SampleIrradiance at the surface point that puts its
//     FIRST probe offset (0.6 cells, the one the vertex shader uses) exactly on
//     that probe point.
//
// Errors are reported in irradiance BYTES (0..255), the units the field is
// stored in. Under Scheme A every direction is stored exactly, so BOTH classes
// must come out at 0.000 and the pass bar covers both. (Under the Scheme B this
// replaced, the +-X/+-Y classes carried a mean of ~9.5 and an RMS of ~21,
// because four directions shared one averaged channel.)
void UVoxelGISubsystem::RunVolumeCheck(int32 NumSamples)
{
	bVolumeCheckDone = true;

	if (!Field || !bVolumeOriginSet || VolumeShadow.Num() == 0)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VOLUMECHECK: nothing staged (originSet=%d shadow=%d) -- needs voxel.GI.Volume 1, ")
		       TEXT("voxel.Stream.GPU 1 and a settled field."),
		       bVolumeOriginSet ? 1 : 0, VolumeShadow.Num());
		return;
	}

	// Flush the whole upload queue first, ignoring the per-frame budget. A
	// diagnostic that measures a volume which is merely BEHIND the field
	// measures the queue, not the encoding.
	const int32 BeforeFlush = VolumeBricksUploaded;
	FlushVolume();
	const int32 Flushed = VolumeBricksUploaded - BeforeFlush;

	TArray<FIntVector> Keys;
	Field->GetResidentKeys(Keys);

	// Only bricks fully inside the volume can be compared: outside it the
	// sampler clamps to the edge texel, which is the sampler doing what it was
	// asked and not a mismatch.
	TArray<FIntVector> InVolume;
	InVolume.Reserve(Keys.Num());
	const int32 E = VoxelLF::BrickEdgeCells;
	for (const FIntVector& Key : Keys)
	{
		const FIntVector Base(Key.X * E - VolumeCellOrigin.X,
		                      Key.Y * E - VolumeCellOrigin.Y,
		                      Key.Z * E - VolumeCellOrigin.Z);
		// One cell of margin on every side, so all 8 trilinear taps of any
		// probe inside the brick are real texels rather than clamped ones.
		if (Base.X >= 1 && Base.Y >= 1 && Base.Z >= 1 &&
		    Base.X + E + 1 <= VolumeDim && Base.Y + E + 1 <= VolumeDim && Base.Z + E + 1 <= VolumeDim)
		{
			InVolume.Add(Key);
		}
	}

	if (InVolume.Num() == 0)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VOLUMECHECK: cells=0 -- %d resident bricks but none inside the volume ")
		       TEXT("(dim=%d covers +/-%.0f UU around (%.0f,%.0f,%.0f)). Raise voxel.GI.VolumeDim."),
		       Keys.Num(), VolumeDim, 0.5 * VolumeDim * VoxelLF::CellSizeUU,
		       VolumeOriginWorldUU.X + 0.5 * VolumeDim * VoxelLF::CellSizeUU,
		       VolumeOriginWorldUU.Y + 0.5 * VolumeDim * VoxelLF::CellSizeUU,
		       VolumeOriginWorldUU.Z + 0.5 * VolumeDim * VoxelLF::CellSizeUU);
		return;
	}

	// Software trilinear over the staged bytes, term for term what
	// Texture3DSampleLevel + the divide in VoxelQuadVertexFactory.ush does.
	// UVW = (P - Origin) * InvSize, so the texel coordinate is
	// UVW*Dim - 0.5 == (P - Origin)/40 - 0.5, which is the CPU sampler's own
	// convention -- no half-texel fixup, because the origin is brick-snapped.
	const uint8* ShadowPos = VolumeShadow.GetData();
	const uint8* ShadowNeg = VolumeShadowNeg.GetData();
	// Slot is now (volume, channel): DirTable index >> 1 is the AXIS and the low
	// bit is the SIGN, matching the +X -X +Y -Y +Z -Z storage order.
	auto SampleShadow = [this, ShadowPos, ShadowNeg](const FVector& P, int32 Dir, float& OutIrr) -> bool
	{
		const double TX = (P.X - VolumeOriginWorldUU.X) / double(VoxelLF::CellSizeUU) - 0.5;
		const double TY = (P.Y - VolumeOriginWorldUU.Y) / double(VoxelLF::CellSizeUU) - 0.5;
		const double TZ = (P.Z - VolumeOriginWorldUU.Z) / double(VoxelLF::CellSizeUU) - 0.5;
		const int32 BX = int32(FMath::FloorToDouble(TX));
		const int32 BY = int32(FMath::FloorToDouble(TY));
		const int32 BZ = int32(FMath::FloorToDouble(TZ));
		const float FX = float(TX - double(BX));
		const float FY = float(TY - double(BY));
		const float FZ = float(TZ - double(BZ));

		const uint8* Shadow = ((Dir & 1) == 0) ? ShadowPos : ShadowNeg;
		const int32 Channel = Dir >> 1;

		float AccumRGB = 0.f;
		float AccumA = 0.f;
		for (int32 T = 0; T < 8; ++T)
		{
			const int32 OX = T & 1, OY = (T >> 1) & 1, OZ = (T >> 2) & 1;
			const float W = (OX ? FX : 1.f - FX) * (OY ? FY : 1.f - FY) * (OZ ? FZ : 1.f - FZ);
			if (W <= 0.f)
			{
				continue;
			}
			// AM_Clamp on all three axes, matching the sampler state.
			const int32 CX = FMath::Clamp(BX + OX, 0, VolumeDim - 1);
			const int32 CY = FMath::Clamp(BY + OY, 0, VolumeDim - 1);
			const int32 CZ = FMath::Clamp(BZ + OZ, 0, VolumeDim - 1);
			const uint8* Texel = Shadow + ((int64(CZ) * VolumeDim + CY) * VolumeDim + CX) * 4;
			AccumRGB += W * float(Texel[Channel]) * (1.f / 255.f);
			AccumA += W * float(Texel[3]) * (1.f / 255.f);
		}
		if (AccumA <= 1.e-4f)
		{
			return false; // the shader's "W ~= 0 -> plain AO" branch
		}
		OutIrr = AccumRGB / AccumA;
		return true;
	};

	// Same offset the vertex shader pushes the probe out by (kGIProbeOffsetUU
	// = 0.6 * 40), which is also SampleIrradiance's FIRST probe offset. Feeding
	// the CPU side a surface point P - N*24 makes its first probe land exactly
	// on P, so both sides read the same 8 taps. Samples where that first probe
	// misses are counted, not silently averaged in: the CPU would have walked
	// on to 1.25 and 2.0 cells and the two sides would no longer be comparing
	// the same point.
	constexpr float kProbeOffsetUU = 0.6f * float(VoxelLF::CellSizeUU);

	struct FClassStats
	{
		int32 Count = 0;
		double SumAbsErr = 0.0;
		double SumSqErr = 0.0;
		double MaxAbsErr = 0.0;
		// Kept so the TAIL can be reported, not just the mean. Step 3's decision
		// turns on the horizontal class, whose mean passed its bar at ~6 bytes
		// while maxAbsErr sat at ~102 -- a mean that low with a max that high is
		// either a thin tail (fine, ship Scheme B) or a fat one (a visible defect
		// hiding behind an average), and only percentiles tell those apart.
		TArray<double> Samples;
		void Add(double ErrBytes)
		{
			++Count;
			SumAbsErr += ErrBytes;
			SumSqErr += ErrBytes * ErrBytes;
			MaxAbsErr = FMath::Max(MaxAbsErr, ErrBytes);
			Samples.Add(ErrBytes);
		}
		double Mean() const { return Count > 0 ? SumAbsErr / Count : 0.0; }
		// §2 asked for an RMS and the transcript reported a mean-abs. RMS is the
		// stricter statistic (it weights the tail), so reporting both is what
		// makes the two comparable rather than merely both present.
		double Rms() const { return Count > 0 ? FMath::Sqrt(SumSqErr / Count) : 0.0; }
		double Percentile(double P)
		{
			if (Samples.Num() == 0) { return 0.0; }
			Samples.Sort();
			const int32 Idx = FMath::Clamp(int32(P * double(Samples.Num() - 1) + 0.5), 0, Samples.Num() - 1);
			return Samples[Idx];
		}
		// Share of samples above the design doc's "free quality" bar of 8/255.
		double FractionAbove(double Bytes) const
		{
			if (Count == 0) { return 0.0; }
			int32 N = 0;
			for (double S : Samples) { if (S > Bytes) { ++N; } }
			return double(N) / double(Count);
		}
	};
	// Under Scheme A BOTH classes are exact by construction, so both must read
	// 0.000. They are still reported separately because that is what makes the
	// switch from Scheme B legible: the horizontal line is the one that used to
	// carry a mean of ~9.5 and an RMS of ~21, and seeing it go to zero is the
	// measurement that the encoding actually changed.
	FClassStats StatsZ;   // +-Z normals
	FClassStats StatsXY;  // +-X/+-Y normals
	// NEGATIVE CONTROL. Scheme B is exact on +-Z by construction, so a correct
	// implementation reports 0.000 -- and a harness that reports 0.000 because
	// it is comparing a thing against itself reports exactly the same number.
	// This runs the identical comparison with the volume probe displaced HALF A
	// CELL in X, i.e. the smallest addressing mistake the origin snap exists to
	// prevent. It must come out large. If it does not, the measurement above is
	// worthless and nothing else in this log line should be believed.
	FClassStats StatsControl;
	int32 CpuMiss = 0;    // volume had data, field's first probe did not
	int32 VolMiss = 0;    // field had data, volume did not (upload lag or a hole)
	int32 Attempts = 0;
	int32 Unsolved = 0;
	// How much dynamic range the +-Z measurement actually had. A field of
	// uniformly-1.0 irradiance -- which is what open sky looks like, since an
	// unoccluded cell solves to exactly 1.0 -- would make any encoding score
	// 0.000 regardless of whether it works. Reported so "meanAbsErr=0.000" can
	// be read together with the spread of the values it was computed over.
	double SignalSum = 0.0;
	double SignalSumSq = 0.0;
	double SignalMin = 1.0;

	// Deterministic stream, so two runs of the same build compare like for like.
	FRandomStream Rand(0x5EED0002);
	const int32 Target = FMath::Clamp(NumSamples, 1, 1 << 20);
	const int32 MaxAttempts = Target * 32;

	{
		const FVoxelLightField::FReadScope Read(*Field);
		while (StatsZ.Count + StatsXY.Count < Target && Attempts < MaxAttempts)
		{
			++Attempts;
			const FIntVector Key = InVolume[Rand.RandHelper(InVolume.Num())];
			const FVoxelLFBrick* Brick = Read.FindBrick(Key);
			if (!Brick || !Brick->bSolved)
			{
				continue;
			}
			const int32 CX = Rand.RandHelper(E);
			const int32 CY = Rand.RandHelper(E);
			const int32 CZ = Rand.RandHelper(E);
			if (!Brick->SolvedCells[VoxelLF::CellIndex(CX, CY, CZ)])
			{
				++Unsolved;
				continue;
			}

			// Cell centre, jittered inside the cell so trilinear is actually
			// exercised. On a centre exactly, the frac terms are 0 and the
			// filter collapses to one tap.
			const FVector Probe(
				(double(Key.X * E + CX) + 0.5 + (Rand.GetFraction() - 0.5)) * VoxelLF::CellSizeUU,
				(double(Key.Y * E + CY) + 0.5 + (Rand.GetFraction() - 0.5)) * VoxelLF::CellSizeUU,
				(double(Key.Z * E + CZ) + 0.5 + (Rand.GetFraction() - 0.5)) * VoxelLF::CellSizeUU);

			// 0..5 in DirTable order: +X -X +Y -Y +Z -Z. The shader's slot
			// selection is on the world normal's z, so +-Z map to R/G and the
			// four horizontals all map to B.
			const int32 Dir = Rand.RandHelper(VoxelLF::NumDirs);
			const FVector3f Normal = VoxelLF::DirTable[Dir];

			float VolIrr = 0.f;
			const bool bVolOk = SampleShadow(Probe, Dir, VolIrr);

			const FVector Surface = Probe - FVector(Normal) * double(kProbeOffsetUU);
			float FieldIrr = 0.f;
			const bool bFieldOk = Read.Sample(Surface, Normal, FieldIrr);

			if (!bVolOk && !bFieldOk)
			{
				continue; // both fall back to plain AO -- agreement, but no signal
			}
			if (!bVolOk)
			{
				++VolMiss;
				continue;
			}
			if (!bFieldOk)
			{
				++CpuMiss;
				continue;
			}

			const double ErrBytes = FMath::Abs(double(FieldIrr) - double(VolIrr)) * 255.0;
			if (Dir >= 4)
			{
				StatsZ.Add(ErrBytes);
				SignalSum += double(FieldIrr);
				SignalSumSq += double(FieldIrr) * double(FieldIrr);
				SignalMin = FMath::Min(SignalMin, double(FieldIrr));

				float ShiftedIrr = 0.f;
				if (SampleShadow(Probe + FVector(0.5 * VoxelLF::CellSizeUU, 0, 0), Dir, ShiftedIrr))
				{
					StatsControl.Add(FMath::Abs(double(FieldIrr) - double(ShiftedIrr)) * 255.0);
				}
			}
			else
			{
				StatsXY.Add(ErrBytes);
			}
		}
	}

	// --- VolumeLocal arm (docs/sky-and-local-light-plan.md §4, L1's gate) -----
	//
	// SAME METHOD, DIFFERENT QUANTITY, and one deliberate difference in what it is
	// compared against. The comparison is TRILINEAR-VS-TRILINEAR, not
	// trilinear-vs-analytic: the volume stores point samples at texel centres and
	// the shader reads them through hardware trilinear, so the filtered value at an
	// arbitrary point is not the analytic splat there, and comparing those two would
	// report the filter as a bug. Both sides therefore interpolate the SAME 8 texel
	// centres -- one side from the staged bytes, the other recomputed from the live
	// light list -- which is what catches an addressing shift, a wrong local dim and
	// a missed upload.
	//
	// WHAT IT CATCHES OF §5 ITEM 7, STATED HONESTLY, because an instrument credited
	// with a detection it does not have is worse than no instrument. Both sides here
	// are addressed by VolumeOriginWorldUU, the STAGING origin, and RunVolumeCheck
	// calls FlushVolume first -- which runs any re-centre to completion, making
	// staging == committed. So this arm can NOT observe the mid-re-centre frames in
	// which a stale origin is live; nothing on the game thread can, because the
	// mismatch exists only in the uniform buffer the render thread holds.
	//
	// It does catch the DURABLE half, which is the half that matters: delete the
	// RestageLocalZRange call out of RestageVolumeZRange and the mirror keeps bytes
	// written against the PRE-re-centre origin for good (BeginVolumeRecentre drops
	// the splat queue, so nothing else rewrites them), while the reference here is
	// computed against the new one. The error is then a whole re-centre shift and the
	// arm fails loudly. The transient frames are covered instead by the structural
	// argument -- one call, same rows, same frame as the siblings -- and by
	// VOLUMERECENTRE's own transient line at voxel.GI.Debug >= 2.
	//
	// Reports cells=0 and no verdict when no local light is registered. That is not
	// a failure: there is nothing to compare, and a harness that failed on an unlit
	// session would be turned off.
	FClassStats StatsLocal;
	int32 LocalAttempts = 0;
	int32 LocalOutside = 0;
	double LocalSignalSum = 0.0;
	double LocalSignalMax = 0.0;
	if (LocalLights.Num() > 0 && VolumeLocalShadow.Num() > 0 && VolumeLocalDim > 0)
	{
		const double T = LocalTexelUU();
		const int32 LDim = VolumeLocalDim;
		const uint8* LocalBytes = VolumeLocalShadow.GetData();
		const FVoxelLightField::FReadScope Read(*Field);
		FRandomStream LocalRand(0x5EED0003);
		const int32 LocalTarget = FMath::Clamp(Target / 4, 64, 4096);
		const int32 LocalMaxAttempts = LocalTarget * 32;
		while (StatsLocal.Count < LocalTarget && LocalAttempts < LocalMaxAttempts)
		{
			++LocalAttempts;
			const FVoxelLocalLight& Light = LocalLights[LocalRand.RandHelper(LocalLights.Num())];
			// Uniform in the light's bounding cube. Nothing is snapped to a texel
			// centre, for the same reason the main harness jitters: on a centre the
			// frac terms are zero and trilinear degenerates to one tap, which is a
			// test that cannot fail.
			const FVector P(Light.PositionUU.X + (LocalRand.GetFraction() * 2.0 - 1.0) * Light.RadiusUU,
			                Light.PositionUU.Y + (LocalRand.GetFraction() * 2.0 - 1.0) * Light.RadiusUU,
			                Light.PositionUU.Z + (LocalRand.GetFraction() * 2.0 - 1.0) * Light.RadiusUU);

			// UVW = (P - Origin) * InvSize, so the local texel coordinate is
			// UVW*LocalDim - 0.5 == (P - Origin)/T - 0.5 -- the same convention as
			// the irradiance volumes' P/40 - 0.5, which is what one brick-snapped
			// origin serving both volumes means in arithmetic.
			const double TX = (P.X - VolumeOriginWorldUU.X) / T - 0.5;
			const double TY = (P.Y - VolumeOriginWorldUU.Y) / T - 0.5;
			const double TZ = (P.Z - VolumeOriginWorldUU.Z) / T - 0.5;
			const int32 BX = int32(FMath::FloorToDouble(TX));
			const int32 BY = int32(FMath::FloorToDouble(TY));
			const int32 BZ = int32(FMath::FloorToDouble(TZ));
			if (BX < 0 || BY < 0 || BZ < 0 || BX + 1 >= LDim || BY + 1 >= LDim || BZ + 1 >= LDim)
			{
				++LocalOutside; // all 8 taps must be real texels, not clamped ones
				continue;
			}
			const float FX = float(TX - double(BX));
			const float FY = float(TY - double(BY));
			const float FZ = float(TZ - double(BZ));

			float Staged = 0.f;
			float Reference = 0.f;
			for (int32 Tap = 0; Tap < 8; ++Tap)
			{
				const int32 OX = Tap & 1, OY = (Tap >> 1) & 1, OZ = (Tap >> 2) & 1;
				const float W = (OX ? FX : 1.f - FX) * (OY ? FY : 1.f - FY) * (OZ ? FZ : 1.f - FZ);
				if (W <= 0.f)
				{
					continue;
				}
				const int32 CX = BX + OX, CY = BY + OY, CZ = BZ + OZ;
				Staged += W * float(LocalBytes[((int64(CZ) * LDim + CY) * LDim + CX) * 4 + 3]) * (1.f / 255.f);
				const FVector Centre(VolumeOriginWorldUU.X + (double(CX) + 0.5) * T,
				                     VolumeOriginWorldUU.Y + (double(CY) + 0.5) * T,
				                     VolumeOriginWorldUU.Z + (double(CZ) + 0.5) * T);
				Reference += W * LocalUnCrushAt(Centre, Read);
			}

			StatsLocal.Add(FMath::Abs(double(Reference) - double(Staged)) * 255.0);
			LocalSignalSum += double(Reference);
			LocalSignalMax = FMath::Max(LocalSignalMax, double(Reference));
		}
	}

	// THE grep line. Headline is the +-Z class, because that is the one Scheme B
	// claims to reproduce exactly and therefore the one that can falsify the
	// encoding, the addressing, the origin snap and the run merge in one number.
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMECHECK: cells=%d meanAbsErr=%.3f rms=%.3f maxAbsErr=%.3f"),
	       StatsZ.Count, StatsZ.Mean(), StatsZ.Rms(), StatsZ.MaxAbsErr);
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMECHECK horizontal (+-X/+-Y, Scheme A exact channel): cells=%d meanAbsErr=%.3f rms=%.3f ")
	       TEXT("p50=%.3f p90=%.3f p95=%.3f p99=%.3f maxAbsErr=%.3f frac>8/255=%.4f"),
	       StatsXY.Count, StatsXY.Mean(), StatsXY.Rms(),
	       StatsXY.Percentile(0.50), StatsXY.Percentile(0.90), StatsXY.Percentile(0.95),
	       StatsXY.Percentile(0.99), StatsXY.MaxAbsErr, StatsXY.FractionAbove(8.0));
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMECHECK control (+-Z, volume probe shifted half a cell in X -- MUST be large, else the ")
	       TEXT("line above is measuring nothing): cells=%d meanAbsErr=%.3f rms=%.3f maxAbsErr=%.3f"),
	       StatsControl.Count, StatsControl.Mean(), StatsControl.Rms(), StatsControl.MaxAbsErr);
	{
		const double N = FMath::Max(1, StatsZ.Count);
		const double SigMean = SignalSum / N;
		const double SigVar = FMath::Max(0.0, SignalSumSq / N - SigMean * SigMean);
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VOLUMECHECK signal (+-Z sampled irradiance, the range the error above was measured over): ")
		       TEXT("mean=%.1f sd=%.1f min=%.1f bytes"),
		       SigMean * 255.0, FMath::Sqrt(SigVar) * 255.0, SignalMin * 255.0);
	}
	// Pass bar, in irradiance bytes. Under Scheme A EVERY direction is stored
	// exactly, so the bar now covers both classes -- there is no longer a class
	// that is allowed to be approximate, and anything above a byte of mean error
	// on either is an addressing, origin or staging bug.
	//
	// The bar is a MEAN here and that is deliberate, unlike the design doc's
	// Scheme A-vs-B bar which asked for an RMS and was fed a mean-abs for three
	// transcripts running. This one is checking "is the pipeline exact", where a
	// mean of zero and an RMS of zero are the same statement; the RMS is logged
	// beside it anyway so the two can never drift apart unnoticed again.
	constexpr double kPassMeanBytes = 2.0;
	// The local arm, on the SAME bar and folded into the SAME verdict -- but only
	// when it has cells. An unlit session has nothing to say about VolumeLocal, and
	// failing it for that would make the whole harness something people stop
	// running. The signal columns are here for the same reason the +-Z ones are: a
	// mean error of 0.000 over a field of uniform zeros is not evidence.
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMECHECK local (VolumeLocal.A un-crush, trilinear vs trilinear): cells=%d meanAbsErr=%.3f ")
	       TEXT("rms=%.3f maxAbsErr=%.3f | signal mean=%.1f max=%.1f bytes | lights=%d localDim=%d texel=%.0f UU ")
	       TEXT("attempts=%d outsideVolume=%d %s"),
	       StatsLocal.Count, StatsLocal.Mean(), StatsLocal.Rms(), StatsLocal.MaxAbsErr,
	       StatsLocal.Count > 0 ? 255.0 * LocalSignalSum / double(StatsLocal.Count) : 0.0,
	       255.0 * LocalSignalMax,
	       LocalLights.Num(), VolumeLocalDim, LocalTexelUU(), LocalAttempts, LocalOutside,
	       StatsLocal.Count == 0
	           ? TEXT("(no local light registered -- nothing to compare, not a failure)")
	           : (StatsLocal.Mean() < kPassMeanBytes ? TEXT("PASS") : TEXT("FAIL")));

	const bool bPass = StatsZ.Count > 0 && StatsZ.Mean() < kPassMeanBytes
	                && StatsXY.Count > 0 && StatsXY.Mean() < kPassMeanBytes
	                && (StatsLocal.Count == 0 || StatsLocal.Mean() < kPassMeanBytes);
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMECHECK detail: verdict=%s (bar: BOTH classes mean < %.1f bytes) | dim=%d bricksResident=%d bricksInVolume=%d ")
	       TEXT("flushedNow=%d uploadedTotal=%d runsTotal=%d queueLeft=%d | attempts=%d unsolvedCell=%d ")
	       TEXT("volumeMiss=%d fieldFirstProbeMiss=%d | originWorld=(%.0f,%.0f,%.0f)"),
	       bPass ? TEXT("PASS") : TEXT("FAIL"),
	       kPassMeanBytes, VolumeDim, Keys.Num(), InVolume.Num(),
	       Flushed, VolumeBricksUploaded, VolumeRunsUploaded, VolumeUploadQueue.Num(),
	       Attempts, Unsolved, VolMiss, CpuMiss,
	       VolumeOriginWorldUU.X, VolumeOriginWorldUU.Y, VolumeOriginWorldUU.Z);
}

// --- voxel.GI.VolumeDigTest -------------------------------------------------
//
// THE DELIVERABLE FOR TWO STEP-2 CLAIMS THAT WERE "CORRECT BY CONSTRUCTION ONLY".
//
//   * The X-run upload merge exists for the DIG case -- a contiguous 5x5x5 brick
//     neighbourhood -- and the only number ever measured for it was 1.4
//     bricks/run in STEADY STATE, where the round-robin re-solve delivers bricks
//     in TMap iteration order and X-adjacent pairs are rare by construction. That
//     number says nothing about the case the merge was built for.
//   * zero-on-revoxelize: a dug brick must reach the volume as A=0 (which the
//     shader reads as "no data" and turns into plain AO) BEFORE its re-solve
//     lands, or the tunnel keeps its pre-dig lighting for as long as the solve
//     queue is deep. Never observed, only argued.
//   * zero-on-evict: same, for a brick that leaves the resident set.
//
// Method: carve a real sphere through the real edit path at the camera, suppress
// the round-robin re-solve for the duration so the upload queue contains the
// dig's bricks and nothing else, then flush and read the answer out of
// VolumeShadow -- the actual staged bytes, not a re-encode.
void UVoxelGISubsystem::StepDigTest()
{
	if (DigTestPhase == 0)
	{
		return;
	}
	// A runtime toggle-off clears the field out from under an armed test.
	if (!Field || !bVolumeOriginSet || VolumeShadow.Num() == 0)
	{
		DigTestPhase = 0;
		DigTestBricks.Reset();
		return;
	}

	const int32 E = VoxelLF::BrickEdgeCells;
	auto BrickAllZero = [this, E](const FIntVector& Key, int32& OutInside) -> bool
	{
		const FIntVector Base(Key.X * E - VolumeCellOrigin.X,
		                      Key.Y * E - VolumeCellOrigin.Y,
		                      Key.Z * E - VolumeCellOrigin.Z);
		if (Base.X < 0 || Base.Y < 0 || Base.Z < 0 ||
		    Base.X + E > VolumeDim || Base.Y + E > VolumeDim || Base.Z + E > VolumeDim)
		{
			return true; // outside the volume; not counted
		}
		++OutInside;
		for (int32 Z = 0; Z < E; ++Z)
		for (int32 Y = 0; Y < E; ++Y)
		for (int32 X = 0; X < E; ++X)
		{
			const int64 Off = ((int64(Base.Z + Z) * VolumeDim + (Base.Y + Y)) * VolumeDim + (Base.X + X)) * 4;
			if (VolumeShadow[Off + 3] != 0 || VolumeShadowNeg[Off + 3] != 0) { return false; }
		}
		return true;
	};

	const double Now = FPlatformTime::Seconds();

	if (DigTestPhase == 1)
	{
		// Wait until the dig's remeshes have been ingested. The streaming system
		// budgets remeshes per frame, so this is several frames, not one.
		if ((PendingVoxelize.Num() > 0 || PendingPooledVoxelize.Num() > 0)
		    && Now - DigTestPhaseSeconds < 5.0)
		{
			return;
		}

		const int32 BricksBefore = VolumeBricksUploaded;
		const int32 RunsBefore = VolumeRunsUploaded;
		FlushVolume();
		const int32 Bricks = VolumeBricksUploaded - BricksBefore;
		const int32 Runs = VolumeRunsUploaded - RunsBefore;

		int32 Inside = 0;
		int32 Zeroed = 0;
		for (const FIntVector& Key : DigTestBricks)
		{
			int32 WasInside = 0;
			const bool bZero = BrickAllZero(Key, WasInside);
			Inside += WasInside;
			if (WasInside > 0 && bZero) { ++Zeroed; }
		}

		UE_LOG(LogVoxelGI, Log,
		       TEXT("VOLUMEDIG revoxelize: dirtied=%d bricks (%d inside the volume) | uploaded %d bricks in %d runs ")
		       TEXT("= %.2f bricks/run | zeroed=%d/%d %s"),
		       DigTestBricks.Num(), Inside, Bricks, Runs,
		       Runs > 0 ? double(Bricks) / double(Runs) : 0.0,
		       Zeroed, Inside,
		       (Inside > 0 && Zeroed == Inside) ? TEXT("PASS") : TEXT("FAIL"));

		DigTestPhase = 2;
		DigTestPhaseSeconds = Now;
		return;
	}

	if (DigTestPhase == 2)
	{
		// Let the solve land, then confirm the same bricks come BACK -- otherwise
		// "zeroed" would also be the reading for "the upload path is broken".
		if (Now - DigTestPhaseSeconds < 6.0)
		{
			return;
		}
		FlushVolume();
		int32 Inside = 0;
		int32 Relit = 0;
		for (const FIntVector& Key : DigTestBricks)
		{
			int32 WasInside = 0;
			const bool bZero = BrickAllZero(Key, WasInside);
			Inside += WasInside;
			if (WasInside > 0 && !bZero) { ++Relit; }
		}
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VOLUMEDIG resolve: %d/%d bricks carry data again after the solve %s"),
		       Relit, Inside, (Inside > 0 && Relit > 0) ? TEXT("PASS") : TEXT("FAIL"));

		// --- zero-on-evict, through the real eviction path -------------------
		//
		// Driven rather than waited for: eviction is distance-based and at the
		// default radius nothing near the camera ever evicts, so waiting for one
		// would mean flying for a minute and hoping. This calls the same
		// EvictFarBricks the tick calls, with a radius chosen to catch a handful,
		// and pushes the result through the same PushVolumeUpload the tick uses.
		// It is destructive -- the evicted bricks come back only when their chunk
		// re-meshes -- which is why it lives behind a diagnostic command.
		TArray<FIntVector> Evicted;
		const double EvictRadiusUU = 0.35 * 0.5 * double(VolumeDim) * double(VoxelLF::CellSizeUU);
		const int32 NumEvicted = Field->EvictFarBricks(FieldCentreUU, EvictRadiusUU, 32, Evicted);
		for (const FIntVector& Key : Evicted)
		{
			BrickComponents.Remove(Key);
			PushVolumeUpload(Key);
		}
		FlushVolume();
		int32 EInside = 0;
		int32 EZeroed = 0;
		for (const FIntVector& Key : Evicted)
		{
			int32 WasInside = 0;
			const bool bZero = BrickAllZero(Key, WasInside);
			EInside += WasInside;
			if (WasInside > 0 && bZero) { ++EZeroed; }
		}
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VOLUMEDIG evict: evicted=%d (%d inside the volume at radius %.0f UU) zeroed=%d %s"),
		       NumEvicted, EInside, EvictRadiusUU, EZeroed,
		       (EInside > 0 && EZeroed == EInside) ? TEXT("PASS") : TEXT("FAIL (0 inside = inconclusive)"));

		DigTestPhase = 0;
		DigTestBricks.Reset();
		bCoarseDirty = true;
		RefreshRotation.Reset();
		RefreshCursor = 0;
	}
}

// voxel.GI.RelightTest -- the OTHER SIDE of the voxel.GI.MaxChunkRefreshesPerFrame
// trade, measured instead of inferred from the budget arithmetic.
//
// Lowering that budget is the candidate fix for GI's frame-time tail (B7: 3.2x
// hitches at the default 4). But the budget bounds the re-shade drain, so a
// smaller one necessarily means an edit takes longer to become visible. Reporting
// only the hitch improvement would be offering the owner a free lunch that is not
// free; this puts a number on the bill.
//
// Method: carve through the real edit path, then time until BOTH the solve queue
// and the re-shade queue have drained -- i.e. until every brick the edit dirtied
// has been re-solved AND every chunk owning one has had its vertex colours
// rewritten. That is the moment the dig is actually, fully relit on screen.
void UVoxelGISubsystem::StartRelightTest(double RadiusUU)
{
	if (!Field || !VoxelGI::IsEnabled())
	{
		UE_LOG(LogVoxelGI, Warning, TEXT("VOLUMERELIGHT: needs voxel.GI.Enabled 1 and a settled field."));
		return;
	}
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!WorldSub)
	{
		return;
	}

	const FVector CentreUU = FieldCentreUU - FVector(0, 0, 2.0 * double(VoxelLF::BrickEdgeUU));
	const int32 Removed = WorldSub->CarveSphere(CentreUU, RadiusUU, 0.0);

	// THE BRICKS THIS EDIT DIRTIED, not the global queues.
	//
	// Waiting on DirtyQueue/RefreshQueue to empty does not work and the sweep
	// data says so outright: the round-robin re-solve
	// (voxel.GI.RefreshBricksPerFrame) feeds those queues continuously, so in
	// steady state they sit at ~853 entries at refresh 2 and ~1288 at refresh 1
	// and NEVER reach zero. A timer waiting on that would have returned TIMEOUT
	// on every leg -- or worse, on a quieter scene, a plausible number that
	// actually measured the round-robin rather than the edit.
	const FIntVector Centre = FVoxelLightField::WorldToBrick(CentreUU);
	const int32 R = 1 + FMath::CeilToInt(RadiusUU / double(VoxelLF::BrickEdgeUU))
	              + FMath::Clamp(CVarGIEditDirtyRadiusBricks.GetValueOnGameThread(), 0, 4);
	RelightTestBricks.Reset();
	for (int32 DZ = -R; DZ <= R; ++DZ)
	for (int32 DY = -R; DY <= R; ++DY)
	for (int32 DX = -R; DX <= R; ++DX)
	{
		const FIntVector Key = Centre + FIntVector(DX, DY, DZ);
		if (Field->HasBrick(Key))
		{
			RelightTestBricks.Add(Key);
		}
	}

	RelightTestStartSeconds = FPlatformTime::Seconds();
	bRelightTestActive = true;
	RelightTestPeakDirty = 0;
	RelightTestPeakRefresh = 0;
	bRelightWorkSeen = false;
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMERELIGHT: carved %d voxels at (%.0f,%.0f,%.0f) r=%.0f UU; timing to fully relit ")
	       TEXT("(%d resident bricks in the edit's neighbourhood, refreshBudget=%d solveBudget=%d)"),
	       Removed, CentreUU.X, CentreUU.Y, CentreUU.Z, RadiusUU, RelightTestBricks.Num(),
	       CVarGIMaxChunkRefreshesPerFrame.GetValueOnGameThread(),
	       CVarGIMaxBrickSolvesPerFrame.GetValueOnGameThread());
}

void UVoxelGISubsystem::StepRelightTest()
{
	if (!bRelightTestActive)
	{
		return;
	}
	RelightTestPeakDirty = FMath::Max(RelightTestPeakDirty, DirtyQueue.Num());
	RelightTestPeakRefresh = FMath::Max(RelightTestPeakRefresh, RefreshQueue.Num());

	// Done when every brick THIS EDIT dirtied has left both work sets: out of
	// DirtySet means it has been re-solved, out of RefreshSet means the chunk
	// owning it has had its vertex colours rewritten. Other bricks arriving from
	// the round-robin are irrelevant and are deliberately not waited on.
	int32 Outstanding = 0;
	for (const FIntVector& Key : RelightTestBricks)
	{
		if (DirtySet.Contains(Key) || RefreshSet.Contains(Key))
		{
			++Outstanding;
		}
	}
	// WAIT FOR THE WORK TO ARRIVE BEFORE WAITING FOR IT TO DRAIN.
	//
	// Without this the timer returns 0 ms on its first tick and looks entirely
	// plausible. An edit does not dirty a light-field brick synchronously: the
	// carve dirties CHUNKS, which are re-meshed over the next several frames,
	// and only then does SetChunkQuads -> NotifyChunkMeshUpdated ->
	// VoxelizeChunk -> MarkBrickNeighbourhoodDirty put anything in DirtySet. At
	// the instant of the carve, Outstanding is legitimately 0 -- not because the
	// relight finished, but because it has not started.
	//
	// Measured on the first leg: editToFullyRelitMs=0 with peak queues of 0,
	// while the independent per-second log showed the refresh queue going
	// 0 -> 58 -> 0 around the carve. Three plausible sub-second numbers and a
	// wrong verdict is exactly what this would have produced.
	if (Outstanding > 0)
	{
		bRelightWorkSeen = true;
	}
	if (!bRelightWorkSeen)
	{
		// Nothing has arrived yet. Keep the clock running -- the wait IS part of
		// edit-to-relit latency -- but do not let the run end here.
		if (FPlatformTime::Seconds() - RelightTestStartSeconds > 30.0)
		{
			bRelightTestActive = false;
			UE_LOG(LogVoxelGI, Warning,
			       TEXT("VOLUMERELIGHT RESULT: TIMEOUT after 30 s | refreshBudget=%d -- no brick from this ")
			       TEXT("edit ever entered the work sets, so nothing was measured"),
			       CVarGIMaxChunkRefreshesPerFrame.GetValueOnGameThread());
		}
		return;
	}
	if (Outstanding > 0)
	{
		if (FPlatformTime::Seconds() - RelightTestStartSeconds > 30.0)
		{
			bRelightTestActive = false;
			UE_LOG(LogVoxelGI, Warning,
			       TEXT("VOLUMERELIGHT RESULT: TIMEOUT after 30 s | refreshBudget=%d outstanding=%d of %d bricks"),
			       CVarGIMaxChunkRefreshesPerFrame.GetValueOnGameThread(),
			       Outstanding, RelightTestBricks.Num());
		}
		return;
	}

	bRelightTestActive = false;
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMERELIGHT RESULT: editToFullyRelitMs=%.0f | refreshBudget=%d editBricks=%d ")
	       TEXT("peakDirtyQueue=%d peakRefreshQueue=%d"),
	       (FPlatformTime::Seconds() - RelightTestStartSeconds) * 1000.0,
	       CVarGIMaxChunkRefreshesPerFrame.GetValueOnGameThread(),
	       RelightTestBricks.Num(), RelightTestPeakDirty, RelightTestPeakRefresh);
}

void UVoxelGISubsystem::StartDigTest(double RadiusUU)
{
	if (!Field || !VoxelGIVolume::IsEnabled() || !bVolumeOriginSet)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VOLUMEDIG: needs voxel.GI.Enabled 1, voxel.GI.Volume 1, voxel.Stream.GPU 1 and a settled ")
		       TEXT("field (originSet=%d)."), bVolumeOriginSet ? 1 : 0);
		return;
	}
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!WorldSub)
	{
		return;
	}

	// Straight down from the camera, which is where a dig actually goes and, more
	// usefully, is guaranteed to hit solid geometry on a surface spawn.
	const FVector CentreUU = FieldCentreUU - FVector(0, 0, 2.0 * double(VoxelLF::BrickEdgeUU));
	const int32 Removed = WorldSub->CarveSphere(CentreUU, RadiusUU, 0.0);

	// The bricks a dig of this size dirties: the touched neighbourhood plus the
	// voxel.GI.EditDirtyRadiusBricks halo, which is what MarkBrickNeighbourhoodDirty
	// will expand it to.
	const FIntVector Centre = FVoxelLightField::WorldToBrick(CentreUU);
	const int32 R = 1 + FMath::CeilToInt(RadiusUU / double(VoxelLF::BrickEdgeUU));
	DigTestBricks.Reset();
	for (int32 DZ = -R; DZ <= R; ++DZ)
	for (int32 DY = -R; DY <= R; ++DY)
	for (int32 DX = -R; DX <= R; ++DX)
	{
		DigTestBricks.Add(Centre + FIntVector(DX, DY, DZ));
	}

	DigTestPhase = 1;
	DigTestPhaseSeconds = FPlatformTime::Seconds();
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMEDIG: carved %d voxels at (%.0f,%.0f,%.0f) r=%.0f UU; watching %d bricks. ")
	       TEXT("Round-robin re-solve suppressed until the test completes so the run-merge number is the DIG's."),
	       Removed, CentreUU.X, CentreUU.Y, CentreUU.Z, RadiusUU, DigTestBricks.Num());
}

// voxel.GI.LocalLightTest -- THE INSTRUMENT FOR THE L1 GATE.
//
// Places a torch at the camera: one registered local light (the un-crush) plus,
// unless voxel.GI.LocalLightDeferred is 0, one real UPointLightComponent (the
// thing that actually emits). Both halves are needed to photograph anything,
// and that is the point rather than a limitation:
//
//   * the un-crush alone is a BaseColor change and emits nothing, so with
//     voxel.GI.LocalLightDeferred 0 the wall MUST stay dark -- the control arm
//     for §5 item 9;
//   * the deferred light alone is what ships today, and in a cave it lights the
//     wall at 6% of its albedo -- which is the defect L1 exists to fix.
//
// Same shape as voxel.GI.VolumeDigTest and voxel.GI.RelightTest: reached through
// the subsystem, camera-relative, and it logs everything a capture needs to be
// believed or thrown away on evidence.
//
// HONEST NOTE ON WHAT THE UN-CRUSH TOUCHES. Raising VertexColor.G raises the
// surface's response to EVERY light, not only to this torch -- G multiplies
// albedo, and BaseColor is what all deferred lighting is proportional to. In a
// cave the other terms are tiny and the torch dominates, which is why the gate is
// stated in a cave; in open terrain Ambient is already ~1 and the un-crush is a
// no-op. It is still the reason the splat's occlusion march has to agree with
// where the light actually reaches, rather than being merely decorative.
void UVoxelGISubsystem::PlaceLocalLightTestAtCamera(double RadiusUU, float Intensity)
{
	if (!Field || !VoxelGI::IsEnabled() || !VoxelGIVolume::IsEnabled() || !bVolumeOriginSet)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VOLUMELOCAL: needs voxel.GI.Enabled 1, voxel.GI.Volume 1, voxel.Stream.GPU 1 and a ")
		       TEXT("settled field (giEnabled=%d volume=%d originSet=%d)."),
		       VoxelGI::IsEnabled() ? 1 : 0, VoxelGIVolume::IsEnabled() ? 1 : 0, bVolumeOriginSet ? 1 : 0);
		return;
	}
	if (!VoxelGIVolume::IsLocalEnabled())
	{
		// Registered anyway rather than refused: the light list is real state and
		// the switch is read per frame, so turning it on afterwards works. But say
		// so, because "the torch does nothing" with the switch off is the single
		// most likely way to lose an afternoon here.
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VOLUMELOCAL: voxel.GI.LocalLights is 0 -- the light will be registered but NOT splatted ")
		       TEXT("and NOT sampled. Set voxel.GI.LocalLights 1 first."));
	}

	const FVector PosUU = FieldCentreUU;
	// Un-crush peak 1.0, i.e. "fully un-crushed at the core", which is the state a
	// lit surface is supposed to be in. The command's Intensity argument goes to
	// the DEFERRED light, in candelas -- brightness belongs on the light and
	// nowhere else (docs/lighting-weather-plan.md §2.3). Conflating the two is how
	// the un-crush would quietly become a second brightness knob.
	const int32 Id = AddLocalLight(PosUU, RadiusUU, /*un-crush peak*/ 1.0f);
	if (Id == 0)
	{
		return;
	}

	AActor* Spawned = nullptr;
	if (CVarGILocalLightDeferred.GetValueOnGameThread() != 0)
	{
		if (UWorld* World = GetWorld())
		{
			if (APointLight* Torch = World->SpawnActor<APointLight>(PosUU, FRotator::ZeroRotator))
			{
				// MOVABLE. ALight defaults to Stationary and UE refuses to move a
				// Stationary light at runtime -- the same trap the sun rig documents
				// at VoxelSkySubsystem.cpp:1475-1483. It matters here because the
				// torch is spawned at a camera that has usually just teleported.
				Torch->SetMobility(EComponentMobility::Movable);
				if (UPointLightComponent* Comp = Cast<UPointLightComponent>(Torch->GetLightComponent()))
				{
					Comp->SetAttenuationRadius(float(RadiusUU));
					// Candelas explicitly, and the value is a FIRST GUESS to be
					// calibrated against the gate, not a tuned number: the exposure
					// curve was fitted for daylight (VoxelSkySubsystem.cpp), so what
					// reads as "a torch" underground is a measurement, not a
					// derivation. Stated here so the number is not later mistaken for
					// one that was fitted.
					Comp->SetIntensityUnits(ELightUnits::Candelas);
					Comp->SetIntensity(Intensity);
					Comp->MarkRenderStateDirty();
				}
				Spawned = Torch;
			}
		}
	}
	for (FVoxelLocalLight& Light : LocalLights)
	{
		if (Light.Id == Id)
		{
			Light.TestActor = Spawned;
			break;
		}
	}

	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMELOCAL: torch id=%d at (%.0f,%.0f,%.0f) reach=%.0f UU intensity=%.0f cd | localLights=%d ")
	       TEXT("deferredLight=%s occlusion=%d | %d light(s) registered, %d splat box(es) queued"),
	       Id, PosUU.X, PosUU.Y, PosUU.Z, RadiusUU, Intensity,
	       VoxelGIVolume::IsLocalEnabled() ? 1 : 0,
	       Spawned ? TEXT("spawned") : TEXT("NONE (control arm)"),
	       CVarGILocalLightOcclusion.GetValueOnGameThread(),
	       LocalLights.Num(), LocalSplatQueue.Num());
}

// Forces a re-centre by the given number of BRICKS, on whatever field is
// resident right now.
//
// WHY THIS EXISTS. The obvious verification -- fly the scripted flight and watch
// for a pop -- does not work, and the run that showed that is worth recording:
// under `-VoxelPerfFlight=surface` the light field holds **0-12 bricks** against
// 2,212 when settled, with `pendingVox=0(+0 pooled)` throughout. Re-centres fire
// correctly (7 of them, each exactly 8 frames, one-frame origin commit) but every
// one restages an EMPTY volume, so the transient accounting reads 0 of 0 occupied
// texels and proves nothing about a pop. A settled field that is then forced to
// re-centre is the only way to put real data through the staged path.
void UVoxelGISubsystem::ForceVolumeRecentre(int32 ShiftBricks)
{
	if (!Field || !bVolumeOriginSet || VolumeDim <= 0 || bVolumeRecentring)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VOLUMERECENTRE: not ready (originSet=%d dim=%d inFlight=%d)"),
		       bVolumeOriginSet ? 1 : 0, VolumeDim, bVolumeRecentring ? 1 : 0);
		return;
	}
	// Pretend the camera sits ShiftBricks further along +X than it does. Nothing
	// else about the path is faked: BeginVolumeRecentre re-snaps, re-buckets and
	// restages exactly as a real camera move would.
	const FVector Saved = FieldCentreUU;
	FieldCentreUU.X += double(ShiftBricks) * double(VoxelLF::BrickEdgeUU);
	BeginVolumeRecentre();
	FieldCentreUU = Saved;
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMERECENTRE: forced a %d-brick (%.0f UU) shift over a field of %d bricks"),
	       ShiftBricks, double(ShiftBricks) * VoxelLF::BrickEdgeUU, Field->NumBricks());
}

namespace
{
	FAutoConsoleCommandWithWorldAndArgs GVoxelGIVolumeRecentreTestCmd(
		TEXT("voxel.GI.VolumeRecentreTest"),
		TEXT("Force a staged volume re-centre of N bricks (default 8 = 2560 UU, the default dead zone) ")
		TEXT("on the CURRENTLY RESIDENT field, and report the transient. Exists because the scripted ")
		TEXT("flight cannot verify this: under motion the light field holds single-digit bricks, so its ")
		TEXT("re-centres restage an empty volume and cannot show a pop. Needs voxel.GI.Debug 2 for the ")
		TEXT("transient line."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
			[](const TArray<FString>& Args, UWorld* World)
	{
		if (!World) { return; }
		if (UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
		{
			const int32 Bricks = Args.Num() > 0 ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 64) : 8;
			GI->ForceVolumeRecentre(Bricks);
		}
	}));

	FAutoConsoleCommandWithWorldAndArgs GVoxelGIRelightTestCmd(
		TEXT("voxel.GI.RelightTest"),
		TEXT("Carve a sphere below the camera and time how long until it is FULLY relit -- both the brick ")
		TEXT("solve queue and the chunk re-shade queue drained. This is the cost side of lowering ")
		TEXT("voxel.GI.MaxChunkRefreshesPerFrame to cut GI's frame-time tail: a smaller drain means fewer ")
		TEXT("hitches AND a slower relight, and this is the second number. Optional argument: carve radius ")
		TEXT("in UU (default 300). Logs VOLUMERELIGHT RESULT. DESTRUCTIVE: it really digs."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
			[](const TArray<FString>& Args, UWorld* World)
	{
		if (!World) { return; }
		if (UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
		{
			const double RadiusUU = Args.Num() > 0 ? FMath::Clamp(FCString::Atod(*Args[0]), 40.0, 2000.0) : 300.0;
			GI->StartRelightTest(RadiusUU);
		}
	}));

	FAutoConsoleCommandWithWorldAndArgs GVoxelGILocalLightTestCmd(
		TEXT("voxel.GI.LocalLightTest"),
		TEXT("Place a torch at the camera: one registered local light (the un-crush written into ")
		TEXT("VolumeLocal's A channel) plus, unless voxel.GI.LocalLightDeferred is 0, the real point light ")
		TEXT("that emits. THIS IS THE ON ARM OF THE L1 GATE; voxel.GI.LocalLightClear is the off arm, and ")
		TEXT("running both in ONE process is what keeps the comparison inside the 0.00%% within-session ")
		TEXT("screenshot floor instead of the 1.81%% between-session one. Optional arguments: reach in UU ")
		TEXT("(default 1000 = 10 m) and intensity in candelas (default 2000, an unfitted first guess). ")
		TEXT("Needs voxel.GI.Volume 1 and voxel.GI.LocalLights 1."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
			[](const TArray<FString>& Args, UWorld* World)
	{
		if (!World) { return; }
		if (UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
		{
			const double RadiusUU = Args.Num() > 0 ? FMath::Clamp(FCString::Atod(*Args[0]), 40.0, 20000.0) : 1000.0;
			const float Intensity = Args.Num() > 1 ? float(FMath::Clamp(FCString::Atod(*Args[1]), 0.0, 1.0e6)) : 2000.f;
			GI->PlaceLocalLightTestAtCamera(RadiusUU, Intensity);
		}
	}));

	FAutoConsoleCommandWithWorldAndArgs GVoxelGILocalLightClearCmd(
		TEXT("voxel.GI.LocalLightClear"),
		TEXT("Remove every registered local light AND the deferred actors voxel.GI.LocalLightTest spawned ")
		TEXT("for them, then queue VolumeLocal for a full re-splat. The OFF arm of the L1 gate: it has to ")
		TEXT("take both halves away, because leaving the point light behind measures the un-crush against ")
		TEXT("itself and leaving a lit texel behind makes the arm read as a pass."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
			[](const TArray<FString>& Args, UWorld* World)
	{
		if (!World) { return; }
		if (UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
		{
			GI->ClearLocalLights();
		}
	}));

	FAutoConsoleCommandWithWorldAndArgs GVoxelGIVolumeDigTestCmd(
		TEXT("voxel.GI.VolumeDigTest"),
		TEXT("Carve a sphere below the camera and measure the three GPU-volume behaviours that were ")
		TEXT("previously correct-by-construction only: the X-run upload merge on a dig's CONTIGUOUS brick ")
		TEXT("neighbourhood (the case it was built for -- the only number on record, 1.4 bricks/run, is ")
		TEXT("steady state), zero-on-revoxelize, and zero-on-evict. Optional argument: carve radius in UU ")
		TEXT("(default 300). Logs VOLUMEDIG lines. DESTRUCTIVE: it really digs, and it really evicts."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
			[](const TArray<FString>& Args, UWorld* World)
	{
		if (!World) { return; }
		if (UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
		{
			const double RadiusUU = Args.Num() > 0 ? FMath::Clamp(FCString::Atod(*Args[0]), 40.0, 2000.0) : 300.0;
			GI->StartDigTest(RadiusUU);
		}
	}));
}

// --- tick ------------------------------------------------------------------

void UVoxelGISubsystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// --- ARM VERIFICATION, before every other guard -------------------------
	//
	// Deliberately ahead of the !IsEnabled() early-out, because the arm most in
	// need of checking is the OFF one and that arm returns immediately below.
	// IsTickable keeps us running for exactly this.
	//
	// Once a second for the whole leg, not once at startup. GameMode's
	// read-back proves the set took; it cannot prove it survived -ExecCmds
	// landing later in startup with SetByConsole priority. A control arm that
	// gets silently flipped back produces perfectly plausible numbers under the
	// wrong name, which is the most expensive failure this project has on
	// record (Defect 1, armA-drawpath-ceiling-2026-08-19.txt).
	if (ArmRequestedEnabled >= 0)
	{
		const double ArmNow = FPlatformTime::Seconds();
		if (!bArmFirstCheckDone || ArmNow - LastArmCheckSeconds > 1.0)
		{
			LastArmCheckSeconds = ArmNow;
			const int32 Live = VoxelGI::IsEnabled() ? 1 : 0;
			if (Live != ArmRequestedEnabled)
			{
				++ArmViolations;
				UE_LOG(LogVoxelGI, Error,
				       TEXT("GI ARM VIOLATED: -VoxelGI%s asked for voxel.GI.Enabled=%d, live value is ")
				       TEXT("%d (violation #%d). Something outranked the switch AFTER init -- almost ")
				       TEXT("certainly voxel.GI.Enabled in -ExecCmds, which is SetByConsole and beats ")
				       TEXT("the switch's SetByCode. THIS LEG IS VOID as a GI arm: it is running the ")
				       TEXT("other configuration under this arm's name. Do not report its numbers."),
				       ArmRequestedEnabled ? TEXT("On") : TEXT("Off"),
				       ArmRequestedEnabled, Live, ArmViolations);
			}
			else if (!bArmFirstCheckDone)
			{
				// Positive evidence, logged once. "No error appeared" is not the
				// same statement as "the check ran and passed", and only one of
				// those survives a grep of a log whose instrument silently died.
				UE_LOG(LogVoxelGI, Log,
				       TEXT("GI arm: -VoxelGI%s HELD -- voxel.GI.Enabled=%d verified on the first tick, ")
				       TEXT("i.e. after -ExecCmds have landed. Re-checked every second for the run; ")
				       TEXT("any later divergence logs GI ARM VIOLATED."),
				       ArmRequestedEnabled ? TEXT("On") : TEXT("Off"), ArmRequestedEnabled);
			}
			bArmFirstCheckDone = true;
		}
	}

	if (!VoxelGI::IsEnabled())
	{
		if (bHasState)
		{
			ClearAllState(); // runtime toggle-off releases everything in one frame
		}
		return;
	}
	if (!Field)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	const double TickStart = Now;
	FieldCentreUU = ResolveViewOriginUU(bFieldCentreResolved);

	// P7 MARCH ARM. The CPU chain is retired by taking its budgets to ZERO
	// rather than by branching around it.
	//
	// THAT IS A DELIBERATE CHOICE AND NOT LAZINESS. Every phase below is
	// already budget-bounded and every one of them no-ops at a budget of 0, so
	// zeroing them retires the chain without moving a single line of the
	// control arm's code -- the two arms run the same function, and the arm is
	// a VALUE rather than a different path. Branching around the phases would
	// also skip the once-a-second stats block at the end, which is exactly
	// where the arm has to be reported.
	const bool bMarchArm = VoxelGI::IsMarchBricksEnabled();
	if (bMarchArm)
	{
		TickMarchBricks();
	}

	// 1) Voxelize newly meshed chunks (stream-in AND post-edit remesh land in
	//    the same queue -- see NotifyChunkMeshUpdated).
	const int32 VoxelizeBudget = bMarchArm
		? 0 : FMath::Max(0, CVarGIMaxVoxelizePerFrame.GetValueOnGameThread());
	const int32 EditRadius = FMath::Clamp(CVarGIEditDirtyRadiusBricks.GetValueOnGameThread(), 0, 4);
	const double BuildRadiusUU = CVarGIRadiusUU.GetValueOnGameThread();
	const int32 MaxBricks = FMath::Max(64, CVarGIMaxBricks.GetValueOnGameThread());

	// voxMs spans BOTH drain loops (component and pooled) because they share one
	// budget and one counter -- splitting the time but not the budget would give
	// two means over denominators that cannot be added.
	const double VoxStartSeconds = FPlatformTime::Seconds();
	int32 Voxelized = 0;
	while (Voxelized < VoxelizeBudget && PendingVoxelize.Num() > 0)
	{
		TWeakObjectPtr<UVoxelChunkComponent> WeakComp = PendingVoxelize[0];
		PendingVoxelize.RemoveAt(0, 1, EAllowShrinking::No);

		UVoxelChunkComponent* Comp = WeakComp.Get();
		if (!Comp || Comp->GetLevel() != 0)
		{
			continue;
		}
		const FVector OriginUU = Comp->GetComponentLocation();
		if (FVector::Dist(OriginUU, FieldCentreUU) > BuildRadiusUU)
		{
			++VoxRejectedRadius;
			continue; // outside the GI ring; nothing to build
		}
		if (Field->NumBricks() >= MaxBricks && !Field->HasBrick(FVoxelLightField::WorldToBrick(OriginUU)))
		{
			continue; // at the memory cap and this would be a new brick
		}

		const FIntVector BrickCoord = FVoxelLightField::WorldToBrick(OriginUU);
		Field->VoxelizeChunk(BrickCoord, OriginUU, Comp->GetChunkQuads());
		BrickComponents.Add(BrickCoord, WeakComp);
		MarkBrickNeighbourhoodDirty(BrickCoord, EditRadius);
		// §3.3 row 2: re-voxelized and not yet solved has to ZERO the texels.
		// VoxelizeChunk has just cleared Vis/SolvedCells, so the encode produces
		// all-zero bytes; skipping this would leave a dug tunnel lit with its
		// pre-dig irradiance until the solve landed several frames later.
		PushVolumeUpload(BrickCoord);
		// The occlusion march reads this brick's opacity, so a light whose reach
		// covers it now has a stale splat. This is what makes torchlight follow a
		// tunnel as it is dug rather than staying where the wall used to be.
		MarkLocalLightsDirtyForBrick(BrickCoord);
		bCoarseDirty = true;
		++Voxelized;
	}

	// 1b) Pooled chunks, from the SAME budget. Identical admission rules
	//     (build radius, brick cap) applied to a payload the queue carries
	//     itself instead of reading back off a component -- see
	//     NotifyPooledChunkMeshUpdated. Draining both queues against one
	//     `Voxelized` counter is what keeps voxel.GI.MaxVoxelizePerFrame a
	//     bound on TOTAL work rather than per-renderer work; with
	//     voxel.Stream.GPUMaxLevel splitting the rings, both can be non-empty
	//     in the same frame.
	while (Voxelized < VoxelizeBudget && PendingPooledVoxelize.Num() > 0)
	{
		FPendingPooledChunk Pending = MoveTemp(PendingPooledVoxelize[0]);
		PendingPooledVoxelize.RemoveAt(0, 1, EAllowShrinking::No);

		if (FVector::Dist(Pending.OriginUU, FieldCentreUU) > BuildRadiusUU)
		{
			++VoxRejectedRadius;
			continue; // outside the GI ring; nothing to build
		}
		const FIntVector BrickCoord = FVoxelLightField::WorldToBrick(Pending.OriginUU);
		if (Field->NumBricks() >= MaxBricks && !Field->HasBrick(BrickCoord))
		{
			continue; // at the memory cap and this would be a new brick
		}

		Field->VoxelizeChunk(BrickCoord, Pending.OriginUU, Pending.Quads);
		// No BrickComponents entry: a pooled chunk has no component to
		// re-shade (see the member's comment). Drop any stale entry a
		// component-path residency of this same brick left behind, so the
		// re-shade phase cannot write colours from this brick's new
		// irradiance into a component that no longer owns it -- only reachable
		// via a mid-session voxel.Stream.GPU flip, but free to get right.
		BrickComponents.Remove(BrickCoord);
		MarkBrickNeighbourhoodDirty(BrickCoord, EditRadius);
		PushVolumeUpload(BrickCoord); // zero-on-revoxelize, as above
		MarkLocalLightsDirtyForBrick(BrickCoord); // stale occlusion march, as above
		bCoarseDirty = true;
		++Voxelized;
	}

	StatVoxMs += (FPlatformTime::Seconds() - VoxStartSeconds) * 1000.0;
	StatVoxChunks += Voxelized;

	// 2) Coarse pyramid rebuild. Full rebuild rather than incremental because
	//    MAX aggregation is not invertible under removal, and O(NumBricks)
	//    map inserts a few times a second is far cheaper than getting
	//    decremental max bookkeeping wrong.
	//
	//    TIMED SEPARATELY because it is the one phase here whose cost scales
	//    with the RESIDENT SET rather than with the frame's work: it is
	//    O(NumBricks) TMap inserts across three levels under the field's write
	//    lock, fired at most every kCoarseRebuildIntervalFrames but fired in
	//    full when it fires. That is a tail-shaped cost, and the tail is the
	//    half of GI's measured cost that was never attributed.
	++FramesSinceCoarseRebuild;
	if (bCoarseDirty && FramesSinceCoarseRebuild >= kCoarseRebuildIntervalFrames)
	{
		const double CoarseStartSeconds = FPlatformTime::Seconds();
		Field->RebuildCoarse();
		StatCoarseMs += (FPlatformTime::Seconds() - CoarseStartSeconds) * 1000.0;
		++StatCoarseRuns;
		bCoarseDirty = false;
		FramesSinceCoarseRebuild = 0;
	}

	// 3) Solve a bounded slice of the dirty queue, plus a slow round-robin
	//    refresh so the progressive bounce converges and long-resident bricks
	//    do not go stale.
	FVoxelGISolveParams Params;
	Params.MaxConeDistanceUU = CVarGIConeDistanceUU.GetValueOnGameThread();
	Params.BounceAlbedo = CVarGIBounceAlbedo.GetValueOnGameThread();
	Params.bLegacyConeBasis = CVarGILegacyConeBasis.GetValueOnGameThread() != 0;

	TArray<FIntVector> ToSolve;
	const int32 SolveBudget = bMarchArm
		? 0 : FMath::Max(0, CVarGIMaxBrickSolvesPerFrame.GetValueOnGameThread());
	while (ToSolve.Num() < SolveBudget && DirtyQueue.Num() > 0)
	{
		const FIntVector Key = DirtyQueue[0];
		DirtyQueue.RemoveAt(0, 1, EAllowShrinking::No);
		DirtySet.Remove(Key);
		if (Field->HasBrick(Key))
		{
			ToSolve.Add(Key);
		}
	}

	// Suppressed for the duration of voxel.GI.VolumeDigTest: the round-robin
	// delivers bricks in TMap iteration order, and mixing those into the upload
	// queue is exactly what makes the steady-state 1.4 bricks/run number
	// uninformative about the dig case.
	const int32 RefreshBudget = (DigTestPhase != 0 || bMarchArm)
		? 0 : FMath::Max(0, CVarGIRefreshBricksPerFrame.GetValueOnGameThread());
	if (RefreshBudget > 0)
	{
		if (RefreshCursor >= RefreshRotation.Num())
		{
			Field->GetResidentKeys(RefreshRotation);
			RefreshCursor = 0;
		}
		for (int32 I = 0; I < RefreshBudget && RefreshCursor < RefreshRotation.Num(); ++I, ++RefreshCursor)
		{
			const FIntVector Key = RefreshRotation[RefreshCursor];
			if (Field->HasBrick(Key) && !ToSolve.Contains(Key))
			{
				ToSolve.Add(Key);
			}
		}
	}

	int32 CellsSolved = 0;
	double SolveMs = 0.0;
	if (ToSolve.Num() > 0)
	{
		const double SolveStart = FPlatformTime::Seconds();
		CellsSolved = Field->SolveBricks(ToSolve, Params);
		SolveMs = (FPlatformTime::Seconds() - SolveStart) * 1000.0;
		// SolveMs stays as the existing per-frame value on the "GI:" line (kept
		// so nothing parsing that line changes); these accumulate the same work
		// across the whole second for the "GI phase:" line, because a
		// once-per-second print of a per-frame value is a 1-in-60 sample of a
		// bursty workload.
		StatSolveMs += SolveMs;
		StatSolveBricks += ToSolve.Num();

		for (const FIntVector& Key : ToSolve)
		{
			// ONLY bricks a component actually owns go on the re-shade queue.
			//
			// Step 5's cost retirement, and it is smaller and more specific than
			// the roadmap claimed. Pooled bricks have no BrickComponents entry by
			// design (see the member's comment), so every pop of one was
			// guaranteed to miss: under voxel.Stream.GPU 1 the queue filled with
			// ~2,000 entries that could only ever be popped and discarded, which
			// is what forced the 8x pop cap below. Not enqueueing them at all
			// deletes the queue growth, the RemoveAt(0) memmove and the TSet
			// churn outright, and the pop cap stops being load-bearing.
			//
			// This is NOT "the volume replaces a re-shade": there was never a
			// re-shade to replace on the pooled path. What the volume replaces is
			// the ABSENCE of one.
			if (BrickComponents.Contains(Key))
			{
				bool bAlready = false;
				RefreshSet.Add(Key, &bAlready);
				if (!bAlready)
				{
					RefreshQueue.Add(Key);
				}
			}
			// §3.3 row 1: brick solved -> write Vis*v, v. Same work list as the
			// component path's re-shade, separate cursor (see the member decl).
			PushVolumeUpload(Key);
		}
	}

	// 4) Re-shade chunks whose irradiance changed.
	//
	//    THIS USED TO BE MarkRenderStateDirty(), i.e. a full scene-proxy
	//    rebuild per chunk: it regenerated positions, tangents, world-planar
	//    UVs and indices that had not changed, allocated five fresh RHI
	//    buffers, and pushed a primitive remove+add through the scene, all to
	//    alter ONE BYTE per vertex (VertexColor.G).
	//
	//    MEASURED WORTH, and it is less than the standing "~1.7 ms, the
	//    dominant cost" note claimed. Interleaved 4-round A/B from one binary
	//    (voxel.GI.LegacyProxyRebuild, 2026-07-22, contended box): the rebuild
	//    costs about 0.15 ms p50 and 0.72 ms p95 over the in-place update, and
	//    -- the more interesting part -- 15 post-warmup hitches against 3, plus
	//    a visible downward drift in chunks/s as a run progresses that the
	//    in-place path does not show. So the proxy churn was mostly a TAIL and
	//    throughput problem, not a median one. GI's median cost lives
	//    elsewhere: see the attribution note on
	//    voxel.GI.MaxQuadSpanVoxels.
	//
	//    UpdateGIVertexColors instead recomputes only the colour stream and
	//    memcpys it into the existing proxy's colour vertex buffer (see
	//    VoxelChunkComponent.cpp). Geometry is identical across a GI re-shade
	//    by construction: this queue only ever holds chunks whose IRRADIANCE
	//    changed -- a chunk whose geometry changed arrives through
	//    SetChunkQuads, which already rebuilds the proxy for its own reasons.
	//    MarkRenderStateDirty remains the fallback for the cases the fast path
	//    declines (no proxy yet), so behaviour is unchanged where it cannot
	//    apply.
	const int32 RefreshChunkBudget = FMath::Max(0, CVarGIMaxChunkRefreshesPerFrame.GetValueOnGameThread());
	// Read from the COMMAND LINE as well as the cvar: an -ExecCmds cvar lands
	// after streaming has already begun, so an -ExecCmds A/B silently measures
	// the same state twice (this cost three runs to discover on -VoxelGIOn).
	static const bool bLegacyRefreshCmdLine = FParse::Param(FCommandLine::Get(), TEXT("VoxelGILegacyRefresh"));
	const bool bLegacyProxyRebuild = bLegacyRefreshCmdLine || CVarGILegacyProxyRebuild.GetValueOnGameThread() != 0;

	// PER-COMPONENT dedupe, on top of the queue's existing per-BRICK dedupe.
	//
	// The queue is keyed by brick, but re-shading is per CHUNK and always
	// recomputes the chunk's WHOLE colour array. A chunk covers many bricks, so
	// a single edit -- which dirties a 5x5x5 brick neighbourhood by default
	// (voxel.GI.EditDirtyRadiusBricks) -- used to pop several bricks that all
	// resolve to the same component and pay for that chunk's full re-shade once
	// EACH. The second and subsequent recomputes of a frame read the same field
	// state and produce the same bytes, so they were pure waste.
	//
	// Skipped duplicates deliberately do NOT consume the frame's budget: the
	// budget exists to bound work done, and a dedupe hit does none.
	//
	// POPS are bounded as well as refreshes, which they were not before the
	// pooled path existed. A pooled brick has no BrickComponents entry, so
	// every pop of one misses and `Refreshed` never advances -- unbounded, this
	// loop would RemoveAt(0) its way through the whole ~2000-entry queue every
	// frame, an O(n^2) memmove producing no output at all. 8x the refresh
	// budget leaves the dedupe-skip case (the reason misses deliberately do not
	// consume the refresh budget) behaving exactly as before on the component
	// path, where the queue is short and the bound never binds.
	TSet<UVoxelChunkComponent*> RefreshedThisFrame;
	int32 Refreshed = 0;
	int32 DedupeSkips = 0;
	int32 Popped = 0;
	const int32 MaxPops = FMath::Max(8 * RefreshChunkBudget, 32);
	while (Refreshed < RefreshChunkBudget && Popped < MaxPops && RefreshQueue.Num() > 0)
	{
		++Popped;
		const FIntVector Key = RefreshQueue[0];
		RefreshQueue.RemoveAt(0, 1, EAllowShrinking::No);
		RefreshSet.Remove(Key);
		if (TWeakObjectPtr<UVoxelChunkComponent>* Found = BrickComponents.Find(Key))
		{
			if (UVoxelChunkComponent* Comp = Found->Get())
			{
				bool bAlreadyRefreshed = false;
				RefreshedThisFrame.Add(Comp, &bAlreadyRefreshed);
				if (bAlreadyRefreshed)
				{
					++DedupeSkips;
					continue;
				}
				if (bLegacyProxyRebuild || !Comp->UpdateGIVertexColors())
				{
					Comp->MarkRenderStateDirty();
				}
				++Refreshed;
			}
			else
			{
				BrickComponents.Remove(Key);
			}
		}
	}

	// 4b) GPU volume texel uploads -- the pooled path's replacement for (4).
	//
	//     A pooled chunk has no component and no per-chunk colour buffer, so
	//     baked per-vertex GI simply does not exist there; its lighting comes
	//     from this volume instead. The two drains run side by side rather than
	//     one replacing the other because voxel.Stream.GPUMaxLevel can put both
	//     renderers in one frame.
	//
	//     Costs nothing with voxel.GI.Volume 0: both PushVolumeUpload and this
	//     drain return on the cvar read before touching the queue.
	VolumeUploadedThisFrame = 0;
	TickVolume();
	StepDigTest();
	// Arm the relight harness once the field has had time to stream in. Uses the
	// same clock the convergence harness does.
	if (bRelightArmed)
	{
		if (FirstTickSeconds == 0.0)
		{
			FirstTickSeconds = Now;
		}
		else if (Now - FirstTickSeconds >= double(RelightAfterSeconds))
		{
			bRelightArmed = false;
			StartRelightTest(300.0);
		}
	}
	StepRelightTest();
	const int32 VolumeUploaded = VolumeUploadedThisFrame;

	// 5) Eviction, at most twice a second. Distance-based rather than
	//    hooked to chunk unload: UVoxelChunkComponent has no unload callback
	//    this module is permitted to add, and level-0 chunks only ever unload
	//    because the camera left, which is exactly what this tests.
	if (Now - LastEvictSeconds > 0.5)
	{
		LastEvictSeconds = Now;
		TArray<FIntVector> Evicted;
		const int32 NumEvicted = Field->EvictFarBricks(FieldCentreUU, BuildRadiusUU * 1.25, 64, Evicted);
		for (const FIntVector& Key : Evicted)
		{
			BrickComponents.Remove(Key);
			// §3.3 row 3: evicted -> zero it. Safe precisely because of the
			// validity channel: A = 0 reads as "no data" and falls back to plain
			// AO, never to black.
			PushVolumeUpload(Key);
			// An evicted brick stops occluding, so any light that could see it has
			// a stale march. The march treats a missing brick as clear, so this
			// makes the splat AGREE with that rather than keeping a shadow cast by
			// geometry the field no longer holds.
			MarkLocalLightsDirtyForBrick(Key);
		}
		if (NumEvicted > 0)
		{
			bCoarseDirty = true;
			RefreshRotation.Reset();
			RefreshCursor = 0;
		}
	}

	// voxel.GI.Debug 2: probe a vertical column above the view origin with a
	// DOWNWARD normal and dump both the field's opacity and its solved
	// irradiance at each step. Added while chasing "the roof slab underside
	// stays fully lit while the enclosed wall darkens correctly" -- guessing at
	// that from screenshots cost two build/run cycles; this answers in one.
	if (VoxelGI::GetDebugLevel() >= 2 && Now - LastStatSeconds > 1.0)
	{
		const FVoxelLightField::FReadScope Read(*Field);
		FString Line;
		for (int32 Step = 0; Step <= 10; ++Step)
		{
			const FVector P = FieldCentreUU + FVector(0, 0, double(Step) * VoxelLF::CellSizeUU);
			float Irr = -1.f;
			const bool bOk = Read.Sample(P, FVector3f(0, 0, -1), Irr);
			Line += FString::Printf(TEXT("[+%dcm %s%.2f] "), Step * VoxelLF::CellSizeUU / 10,
			                        bOk ? TEXT("") : TEXT("MISS "), Irr);
		}
		UE_LOG(LogVoxelGI, Log, TEXT("GI probe (normal -Z, column above view origin): %s"), *Line);
	}

	// 6a) voxel.GI.VolumeCheck -- the field-vs-volume equivalence harness.
	//     Armed by a non-zero cvar, run ONCE, and re-armable by setting it back
	//     to 0. Deferred by VolumeCheckSettleSeconds after the first upload for
	//     the same reason the convergence harness has a settle: a field that has
	//     barely streamed in reports a handful of cells and proves nothing.
	{
		const int32 CheckArm = CVarGIVolumeCheck.GetValueOnGameThread();
		if (CheckArm == 0)
		{
			bVolumeCheckDone = false;
		}
		else if (!bVolumeCheckDone && FirstVolumeUploadSeconds > 0.0 &&
		         Now - FirstVolumeUploadSeconds >= double(CVarGIVolumeCheckSettle.GetValueOnGameThread()))
		{
			RunVolumeCheck(CheckArm > 1 ? CheckArm : kVolumeCheckDefaultSamples);
		}
	}

	// 6) Convergence harness. Last thing in the tick so the field is in its
	//    normal steady state when it runs.
	if (ConvergePasses > 0 && !bConvergeDone)
	{
		if (FirstTickSeconds == 0.0)
		{
			FirstTickSeconds = Now;
		}
		else if (Now - FirstTickSeconds >= double(ConvergeSettleSeconds))
		{
			RunConvergenceHarness();
		}
	}

	// Whole-tick accumulation, taken before the once-per-second logging below so
	// the print's own cost (a string format and a couple of subsystem lookups)
	// is not attributed to GI's steady-state work.
	StatTickMs += (FPlatformTime::Seconds() - TickStart) * 1000.0;
	++StatTickFrames;

	if (VoxelGI::GetDebugLevel() > 0 && Now - LastStatSeconds > 1.0)
	{
		LastStatSeconds = Now;
		// Height above the terrain directly under the view origin, logged beside
		// the brick count because the two together settle finding B-M in one
		// line: voxel.GI.RadiusUU is 7000 UU, so a camera more than that above
		// the surface has NO level-0 chunk in range and the field empties out
		// through the rejection counter rather than through a streaming fault.
		// The -VoxelPerfFlight=surface fixture pins Z for a whole 100 m circle
		// (VoxelPerfRunSubsystem.cpp: FixedHeightUU is computed once at the
		// circle centre), and its own underground counterpart warns that the
		// surface can move more than 60 m over that circle -- so this is a
		// property of the HARNESS that any measurement taken with it inherits.
		// -1 also means "not computed", which is what an unresolved centre now
		// reports. GetSurfaceHeightUU is NOT a cheap read: it requests a fine
		// tile footprint (disk I/O, exclusive sampler lock) and the fine-tier
		// gate is FATAL on an unattended run when the tile is not baked. At an
		// unresolved (0,0) centre the amplifier's column stencil reaches fine
		// pixel (-2,-2) -> tile (-1,-1), which is not baked for this seed, and
		// that killed g0-gion-r1 on frame 0. A once-per-second DIAGNOSTIC must
		// never be able to end a run, so it does not ask unless the question is
		// well posed.
		//
		// MOVED BEHIND Debug >= 2, AND THAT IS HARDENING RATHER THAN A
		// WORKAROUND FOR THE ZERO CENTRE ABOVE. Fixing the centre removes the
		// reason this probe pointed at the origin; it does NOT remove the
		// probe's ability to end a run. A camera legitimately over an unbaked
		// fine tile -- a pose someone picks to look at exactly that -- would
		// fatal identically, and the tick order that saved us here (VoxelPerfRun
		// happens to tick before this subsystem, so the pawn was already posed)
		// is registration order, not a guarantee.
		//
		// So the rule is applied rather than the instance patched: a diagnostic
		// that runs once a second on every leg must not be able to perform a
		// blocking, disk-loading, fatal-capable query. It now runs only when
		// someone explicitly asks for level 2, which is where the rest of this
		// module's expensive probes already live. voxel.GI.Debug 1 -- what a
		// measurement leg passes -- keeps every counter that costs nothing.
		//
		// The counter itself is retained, not deleted: it is what settled
		// finding B-M (the surface flight pins Z and leaves the GI radius), and
		// deleting the instrument that answered a question is how the question
		// gets re-opened.
		double CamAboveSurfaceUU = -1.0;
		const UWorld* HeightWorld =
			(bFieldCentreResolved && VoxelGI::GetDebugLevel() >= 2) ? GetWorld() : nullptr;
		if (HeightWorld)
		{
			if (const UVoxelWorldSubsystem* Terrain = HeightWorld->GetSubsystem<UVoxelWorldSubsystem>())
			{
				CamAboveSurfaceUU = FieldCentreUU.Z - Terrain->GetSurfaceHeightUU(FieldCentreUU.X, FieldCentreUU.Y);
			}
		}
		UE_LOG(LogVoxelGI, Log,
		       TEXT("GI: bricks=%d (%.1f MB) pendingVox=%d(+%d pooled) rejectedRadius=%d camAboveSurface=%s (radius %.0f) ")
		       TEXT("dirty=%d refresh=%d | solved %d bricks/%d cells in %.2fms, ")
		       TEXT("reshaded %d chunks (%d dupes skipped), volumeUp %d bricks (queue %d, total %d bricks/%d runs), tick %.2fms"),
		       Field->NumBricks(), double(Field->EstimatedBytes()) / (1024.0 * 1024.0),
		       PendingVoxelize.Num(), PendingPooledVoxelize.Num(),
		       VoxRejectedRadius,
		       // NOT a bare -1. A sentinel that formats like a real measurement
		       // is how "the probe did not run" gets read as "the camera is 1 UU
		       // below the surface". Says which it is.
		       CamAboveSurfaceUU >= 0.0
		           ? *FString::Printf(TEXT("%.0f"), CamAboveSurfaceUU)
		           : (bFieldCentreResolved ? TEXT("n/a(needs voxel.GI.Debug 2)") : TEXT("n/a(centre unresolved)")),
		       BuildRadiusUU,
		       DirtyQueue.Num(), RefreshQueue.Num(),
		       ToSolve.Num(), CellsSolved, SolveMs, Refreshed, DedupeSkips,
		       VolumeUploaded, VolumeUploadQueue.Num(), VolumeBricksUploaded, VolumeRunsUploaded,
		       (FPlatformTime::Seconds() - TickStart) * 1000.0);
		VoxRejectedRadius = 0;

		// --- "GI phase:" -- the attribution line ----------------------------
		//
		// The "GI:" line above samples PER-FRAME values on whichever frame the
		// once-a-second timer happens to fire, which for a bursty budgeted
		// workload is a 1-in-60 sample. This line reports the whole interval.
		//
		// ARITHMETIC RULE, and it is the whole reason this comment exists.
		// Every mean below is TOTAL-MS OVER ITS OWN COUNT. The `phases` and
		// `tick` figures are TOTALS IN MILLISECONDS, which are legitimately
		// additive. What is NEVER computed here is a sum of per-item MEANS:
		// the brick stats line did exactly that -- adding a 0.740 ms/chunk term
		// measured over 96 chunks to a 0.227 ms/chunk term measured over 83,671
		// -- and it inflated the result by 3x and reversed the verdict
		// (docs/measurements/armA-drawpath-ceiling-2026-08-19.txt, "THE PRINTED
		// `TOTAL` LINE IS WRONG -- DO NOT QUOTE IT"). Denominators that differ
		// cannot be added, so they are not.
		//
		// `phases` vs `tick` is the useful pair: their difference is the part of
		// the GI tick that no named phase claims. A large unattributed remainder
		// means this instrument is incomplete, and says so instead of hiding it.
		auto MeanMs = [](double TotalMs, int32 Count) -> double
		{
			return Count > 0 ? TotalMs / double(Count) : 0.0;
		};
		const double PhasesMs = StatVoxMs + StatCoarseMs + StatSolveMs + StatUploadMs;
		UE_LOG(LogVoxelGI, Log,
		       TEXT("GI phase: vox %.2fms/%d chunks (%.3f) | coarse %.2fms/%d runs (%.3f) | ")
		       TEXT("solve %.2fms/%d bricks (%.3f) | upload %.2fms/%d bricks (%.3f) | ")
		       TEXT("phases %.2fms of tick %.2fms/%d ticks (%.3f ms/tick, %.2fms unattributed)"),
		       StatVoxMs, StatVoxChunks, MeanMs(StatVoxMs, StatVoxChunks),
		       StatCoarseMs, StatCoarseRuns, MeanMs(StatCoarseMs, StatCoarseRuns),
		       StatSolveMs, StatSolveBricks, MeanMs(StatSolveMs, StatSolveBricks),
		       StatUploadMs, StatUploadBricks, MeanMs(StatUploadMs, StatUploadBricks),
		       PhasesMs, StatTickMs, StatTickFrames, MeanMs(StatTickMs, StatTickFrames),
		       StatTickMs - PhasesMs);

		// --- quadsRetainedForGI ---------------------------------------------
		//
		// The count of level-0 chunks this subsystem pulled OFF the D1
		// direct-to-pool path and onto the GPU readback path, purely to keep
		// their quads for its own ingest. `declined` is the level-0 chunks that
		// were offered and left on the fast path -- so retained+declined is the
		// candidate population and the percentage is meaningful rather than a
		// bare count. Levels 1-5 never reach the radius test and are in neither.
		//
		// BOTH ZERO IS AN INSTRUMENT FAILURE, NOT A GOOD RESULT, and it is
		// called out in the line itself. WantsChunkQuads is reached from
		// FVoxelWorldImpl::SubmitGpuMeshJob; if that call site stops firing --
		// a refactor, a reordered guard, a GPU fork that submitted nothing --
		// the retained count reads a clean, plausible zero that looks exactly
		// like the cost having been removed. This project has shipped probes
		// that ran and measured nothing while reporting success; this one says
		// which of the two it is doing.
		const int32 QuadCandidates = StatQuadsRetainedForGI + StatQuadsDeclined;
		UE_LOG(LogVoxelGI, Log,
		       TEXT("GI ingest: quadsRetainedForGI=%d declined=%d of %d L0 candidates (%s) ")
		       TEXT("-- retained chunks are forced OFF direct-to-pool onto the readback path"),
		       StatQuadsRetainedForGI, StatQuadsDeclined, QuadCandidates,
		       QuadCandidates > 0
		           ? *FString::Printf(TEXT("%.1f%% retained"),
		                              100.0 * double(StatQuadsRetainedForGI) / double(QuadCandidates))
		           : TEXT("NO CANDIDATES OFFERED -- WantsChunkQuads was not called this interval; ")
		             TEXT("this is an instrument failure or a stalled mesh submit, NOT a zero cost"));

		// --- P7-c: WHICH ARM THIS WINDOW IS, next to the numbers it qualifies -
		//
		// Printed on EVERY window, both arms, so an arm can never be inferred
		// from the absence of a line. The two counters are the claim that the
		// arm ENGAGED rather than merely that a cvar is set:
		//
		//   retiredByArm  level-0 candidates WantsChunkQuads declined because
		//                 of the arm. Zero with the arm ON means the gate is
		//                 not being reached -- an instrument failure, not a
		//                 result.
		//   ingestDropped chunks the two Notify hooks refused for the same
		//                 reason. Non-zero on the POOLED path with the arm on
		//                 would mean the demand did not actually go away.
		//
		// THE WARNING IS NOT DECORATION. A leg on this arm has an EMPTY light
		// field: it measures streaming, and its captures look like GI off.
		// tools\voxel-leg-summary.ps1 keys its refusal on this exact string.
		//
		// GREP: "GI arm:"
		const bool bArmRetired = VoxelGI::IsQuadIngestRetired();
		const bool bArmMarch = VoxelGI::IsMarchBricksEnabled();
		UE_LOG(LogVoxelGI, Log,
		       TEXT("GI arm: sourceBricks=%d marchBricks=%d walk=%d retiredByArm=%d ingestDropped=%d ")
		       TEXT("marchBricksSubmitted=%d -- %s"),
		       (bArmRetired && !bArmMarch) ? 1 : 0, bArmMarch ? 1 : 0,
		       bArmMarch ? FMath::Clamp(CVarGIMarchWalk.GetValueOnGameThread(), 0, 2) : -1,
		       StatQuadsRetiredByArm, StatIngestDroppedByArm, StatMarchBricksSubmitted,
		       bArmMarch
		           ? TEXT("GPU CONE MARCH over the resident bricks (P7). The CPU voxelize/solve/encode/")
		             TEXT("upload chain is RETIRED. v1 has NO BOUNCE and BINARY cones, so ENCLOSED SPACES ")
		             TEXT("READ DARKER than the control arm -- that is the arm, not a defect, and it is ")
		             TEXT("for the owner to judge on screenshots. marchBricksSubmitted is the GAME ")
		             TEXT("THREAD's count and is NOT a claim the GPU ran them; the pass refuses ")
		             TEXT("separately and says so where it happens.")
		           : (bArmRetired
		                  ? TEXT("QUAD INGEST RETIRED (P7-c measurement arm): the light field is FED BY ")
		                    TEXT("NOTHING. This window measures STREAMING, not lighting. Any capture from ")
		                    TEXT("it looks like GI off because for lighting purposes it is. DO NOT QUOTE ")
		                    TEXT("IT AS A LIGHTING RESULT.")
		                  : TEXT("control arm: shipping CPU quad ingest")));
		if (bArmMarch && StatMarchBricksSubmitted == 0 && StatIngestDroppedByArm == 0)
		{
			// Both zero on the march arm cannot mean "nothing changed": with GI
			// enabled, level-0 chunks reach the notify hooks whenever anything
			// streams or re-meshes. It means the ingest is not being reached, so
			// no brick was ever queued and the volume is holding whatever it
			// held before -- which looks exactly like a settled, working arm.
			UE_LOG(LogVoxelGI, Warning,
			       TEXT("GI arm: marchBricks=1 but NO bricks were queued OR submitted this interval. ")
			       TEXT("The arm is SET and did not ENGAGE. A stale volume renders as settled lighting, ")
			       TEXT("so do not read this window as the march working."));
		}
		if (bArmRetired && !bArmMarch && StatQuadsRetiredByArm == 0 && StatIngestDroppedByArm == 0)
		{
			// Both zero with the arm ON cannot mean "nothing to retire": with
			// GI enabled at all, level-0 chunks reach WantsChunkQuads whenever
			// the mesh submit path is alive. It means the gate was not reached,
			// and a streaming refund measured against a gate that never ran is
			// a measurement of something else.
			UE_LOG(LogVoxelGI, Warning,
			       TEXT("GI arm: sourceBricks=1 but BOTH arm counters are ZERO this interval. The arm is ")
			       TEXT("SET and did not ENGAGE -- either no mesh work was submitted at all, or the gate "
			            "is no longer reached. Do not read this window's streaming numbers as a refund."));
		}

		// --- P7-a: the anchor, restated once a second ------------------------
		//
		// bVolumeOriginSet is latched once per session, so "did this leg anchor
		// to the terrain pool" is otherwise answerable only from a single line
		// near the top of a 200 MB log. Restating it beside the ingest counters
		// means an arm can be qualified from the same window it is measured in.
		//
		// refusals > 0 with anchored=1 is the ordinary startup wait (the pool is
		// created lazily). refusals CLIMBING with anchored=0 for a whole leg is
		// the defect, and the two are distinguishable only because the count is
		// cumulative and the flag is not.
		//
		// GREP: "GI anchor:"
		UE_LOG(LogVoxelGI, Log,
		       TEXT("GI anchor: anchored=%d identityRefusals=%d poolWorldUU=(%.0f, %.0f, %.0f) ")
		       TEXT("-- selection is by OWNERSHIP + NAME (P7-a), never by TObjectIterator order"),
		       bVolumeOriginSet ? 1 : 0, PoolIdentityRefusals,
		       PoolWorldUU.X, PoolWorldUU.Y, PoolWorldUU.Z);

		StatVoxMs = 0.0;         StatVoxChunks = 0;
		StatCoarseMs = 0.0;      StatCoarseRuns = 0;
		StatSolveMs = 0.0;       StatSolveBricks = 0;
		StatUploadMs = 0.0;      StatUploadBricks = 0;
		StatTickMs = 0.0;        StatTickFrames = 0;
		StatQuadsRetainedForGI = 0;
		StatQuadsDeclined = 0;
		StatQuadsRetiredByArm = 0;
		StatIngestDroppedByArm = 0;
		StatMarchBricksSubmitted = 0;
		// Local lights on their own line, and only when the feature has been armed,
		// so a session with it off (the shipped default) logs byte-identically to
		// one built before this existed. texelsLit is the number that separates
		// "the splat ran" from "the splat ran and found geometry": a torch in a cave
		// lights thousands of texels, and a torch whose march is wrong lights none.
		if (bLocalLightsActive || LocalLights.Num() > 0)
		{
			UE_LOG(LogVoxelGI, Log,
			       TEXT("GI local: lights=%d splatQueue=%d boxes=%d texelsLit=%lld splatMs=%.2f (cumulative) ")
			       TEXT("localDim=%d texel=%.0f UU occlusion=%d"),
			       LocalLights.Num(), LocalSplatQueue.Num(), LocalBoxesSplatted, (long long)LocalTexelsLit,
			       LocalSplatMs, VolumeLocalDim, LocalTexelUU(),
			       CVarGILocalLightOcclusion.GetValueOnGameThread());
		}
	}
}

// --- convergence harness (-VoxelGIConverge=<N>) ------------------------------
//
// THE DELIVERABLE FOR THE ENERGY BUG. "Enclosed spaces creep brighter over
// successive re-solves" was previously only observable as a screenshot luma
// difference between runs (104.7 on one capture, 129.0 on a later one), which
// is both slow to measure and easy to mistake for a code regression -- two
// runs were burned bisecting exactly that. This measures the underlying
// quantity directly and synchronously.
//
// Method: let the field reach its normal streamed steady state, then re-solve
// EVERY resident brick N times back to back with nothing else changing --
// same scene, same geometry, same camera, no streaming in between. The only
// thing varying across passes is the bounce feedback, so this isolates it.
//
// What convergence looks like: the solve is a contraction with modulus
// BounceAlbedo (see VoxelLightField.h), so max|AvgIrr - PrevAvgIrr| must fall
// by roughly 2.5x per pass and the mean must go flat. Divergence -- the bug --
// shows up as a mean that keeps climbing and a delta that does not shrink.
void UVoxelGISubsystem::RunConvergenceHarness()
{
	bConvergeDone = true;

	TArray<FIntVector> AllKeys;
	Field->GetResidentKeys(AllKeys);
	if (AllKeys.Num() == 0)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("GICONVERGE: no resident bricks after %.0fs settle -- nothing to measure. ")
		       TEXT("Raise -VoxelGIConvergeSettle."),
		       ConvergeSettleSeconds);
		FPlatformMisc::RequestExit(false);
		return;
	}

	FVoxelGISolveParams Params;
	Params.MaxConeDistanceUU = CVarGIConeDistanceUU.GetValueOnGameThread();
	Params.BounceAlbedo = CVarGIBounceAlbedo.GetValueOnGameThread();
	Params.bLegacyConeBasis = CVarGILegacyConeBasis.GetValueOnGameThread() != 0;

	Field->SetLegacyInPlaceGather(bConvergeLegacy);

	if (bConvergeSeed)
	{
		const uint8 SeedByte = uint8(FMath::Clamp(ConvergeSeedValue, 0, 255));
		Field->SeedIrradiance(SeedByte);
		UE_LOG(LogVoxelGI, Log, TEXT("GICONVERGE: seeded every solved cell to %d/255 (%s start)."),
		       int32(SeedByte), SeedByte >= 128 ? TEXT("HOT") : TEXT("COLD"));
	}

	UE_LOG(LogVoxelGI, Log,
	       TEXT("GICONVERGE: begin -- %d resident bricks, %d passes, albedo %.2f, seed=%s, mode=%s"),
	       AllKeys.Num(), ConvergePasses, Params.BounceAlbedo,
	       bConvergeSeed ? *FString::Printf(TEXT("%d"), ConvergeSeedValue) : TEXT("none"),
	       bConvergeLegacy ? TEXT("LEGACY in-place gather (pre-fix)") : TEXT("Jacobi published gather (fixed)"));

	double PrevMean = 0.0;
	double PrevDelta = 0.0;
	double FirstMean = 0.0;
	double LastMean = 0.0;
	double LastDelta = 0.0;
	double MaxMeanRise = 0.0;
	double LastMeanRise = 0.0;

	for (int32 Pass = 1; Pass <= ConvergePasses; ++Pass)
	{
		FVoxelGIPassStats Stats;
		const double PassStart = FPlatformTime::Seconds();
		Field->SolveBricks(AllKeys, Params, &Stats);
		const double PassMs = (FPlatformTime::Seconds() - PassStart) * 1000.0;

		const double MeanByte = Stats.MeanIrr * 255.0;
		const double DeltaByte = Stats.MaxAbsDelta * 255.0;
		const double MeanRise = Pass == 1 ? 0.0 : MeanByte - PrevMean;
		// Ratio of successive deltas: the contraction modulus, measured. The
		// theory says this should sit at or below BounceAlbedo.
		const double Ratio = (Pass > 1 && PrevDelta > 0.0) ? DeltaByte / PrevDelta : 0.0;

		UE_LOG(LogVoxelGI, Log,
		       TEXT("GICONVERGE pass %2d/%d: cells=%d meanIrr=%7.3f (d %+7.3f) maxDelta=%7.3f ratio=%5.3f  [%.0f ms]"),
		       Pass, ConvergePasses, Stats.CellsSolved, MeanByte, MeanRise, DeltaByte, Ratio, PassMs);

		if (Pass == 1)
		{
			FirstMean = MeanByte;
		}
		else
		{
			MaxMeanRise = FMath::Max(MaxMeanRise, MeanRise);
		}
		LastMeanRise = MeanRise;
		PrevMean = MeanByte;
		PrevDelta = DeltaByte;
		LastMean = MeanByte;
		LastDelta = DeltaByte;
	}

	// Verdict. Thresholds are in 0..255 irradiance bytes, the same units the
	// field is stored in, so "1.0" means the whole field has reached its
	// byte-quantized fixed point.
	//
	// Both conditions matter and they fail differently: a field that is still
	// climbing fails the drift test, while one that oscillates (the signature
	// of a feedback loop with gain >= 1) fails the delta test even though its
	// mean might look stable.
	constexpr double kDeltaSettled = 1.5; // bytes
	constexpr double kMeanDrift = 0.5;    // bytes per pass, late passes
	const bool bDeltaOk = LastDelta <= kDeltaSettled;
	const bool bDriftOk = FMath::Abs(LastMeanRise) <= kMeanDrift;
	const bool bPass = bDeltaOk && bDriftOk;

	UE_LOG(LogVoxelGI, Log,
	       TEXT("GICONVERGE RESULT: %s -- mode=%s bricks=%d passes=%d | meanIrr %.3f -> %.3f (total drift %+.3f, ")
	       TEXT("largest single-pass rise %+.3f) | final maxDelta %.3f (threshold %.1f)"),
	       bPass ? TEXT("CONVERGED") : TEXT("NOT CONVERGED"),
	       bConvergeLegacy ? TEXT("LEGACY") : TEXT("FIXED"),
	       AllKeys.Num(), ConvergePasses, FirstMean, LastMean, LastMean - FirstMean, MaxMeanRise,
	       LastDelta, kDeltaSettled);

	Field->SetLegacyInPlaceGather(false);
	FPlatformMisc::RequestExit(false);
}
