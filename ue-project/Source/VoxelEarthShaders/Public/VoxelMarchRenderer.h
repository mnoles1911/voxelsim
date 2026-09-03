// VoxelMarchRenderer.h -- THE MARCHER THAT DRAWS.
//
// P3 of docs/ray-marching-plan-2026-08-19.md. Everything before this phase
// measured; this is the first thing that puts marched terrain into the frame
// the engine lights, shadows, occludes, temporally accumulates and traces
// against. VoxelMarch.usf is the shader side and carries the GBuffer channel
// table, the DBuffer application and the velocity arithmetic; this header owns
// the SEAM -- which engine hooks, in which order, and why those and not others.
//
// ===========================================================================
// 1. THE BASE PASS CANNOT WRITE DEPTH. THIS IS PROVABLE, NOT A PREFERENCE.
// ===========================================================================
//
// With Nanite on, ShouldForceFullDepthPass puts the base pass in
// FExclusiveDepthStencil::DepthRead_StencilWrite (RendererScene.cpp:4408-4423,
// DeferredShadingRenderer.cpp:2104-2113). A screen-quad primitive writing
// SV_Depth from a material in the base pass is therefore a dead end: the
// binding it would have to write through is read-only for the whole pass, and
// no amount of material-graph cleverness changes the exclusive-depth-stencil
// state the renderer chose three hundred lines earlier.
//
// So the work SPLITS ACROSS TWO HOOKS, which is the shape the engine itself
// ships on this platform:
//
//   PreRenderView_RenderThread             stash view + prev matrices, per view
//     (SceneViewExtension.h:180)
//
//   PreRenderBasePass_RenderThread         [cs] tile classify  -> indirect args
//     (DeferredShadingRenderer.cpp:2791)   [cs] march          -> VisBuffer + t
//                                          reads prepass SceneDepth for t_max
//
//   ... engine: DBuffer decals, shadow maps, Lumen scene lighting, base pass ...
//
//   PostRenderBasePassDeferred_RenderThread  [raster, INDIRECT over hit tiles]
//     (BasePassRendering.cpp:1149-1156)      SV_Depth + stencil, GBufferA..E,
//                                            SceneColor, Velocity
//
// VERIFIED AGAINST THE 5.8 SOURCE, and one thing the plan did not say turns out
// to matter enormously: PostRenderBasePassDeferred_RenderThread is handed the
// base pass's own FRenderTargetBindingSlots AND the scene-texture uniform
// buffer, by value, in its signature:
//
//   virtual void PostRenderBasePassDeferred_RenderThread(
//       FRDGBuilder& GraphBuilder, FSceneView& InView,
//       const FRenderTargetBindingSlots& RenderTargets,
//       TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) {}
//
// That is the GBuffer MRT set handed to a non-engine module by the engine, and
// its DepthStencil member is the scene depth this emit has to beat and then
// write -- which is what the argument is actually used for below.
//
// WE DO NOT REUSE ITS COLOUR SLOT INDICES, and the reason is worth stating here
// because "assign the whole struct" is the obvious thing to do and the engine's
// own DebugProbeRendering.cpp does exactly that. Those indices are DATA-DRIVEN
// (FSceneTextures::GetGBufferRenderTargets reads Config.GBufferBindings, and
// r.VelocityOutputPass 1 moves GBufferD and E by one slot), while an HLSL
// SV_Target index is a compile-time constant. A shader written against a
// runtime-decided layout can be plausible; it cannot be correct. So the emit
// builds its OWN slots, at fixed indices, from the textures themselves -- which
// FSceneTextureUniformParameters exposes publicly. We are not participating in
// the base pass's MRT set; we are a later pass writing the same textures, and
// "the same textures deferred lighting reads" is the only property that has to
// hold.
//
// PreRenderBasePass_RenderThread, by contrast, is handed NO VIEW -- only
// (FRDGBuilder&, bool bDepthBufferIsPopulated). That is precisely why the plan
// puts a stash at PreRenderView_RenderThread: without it there is no view rect,
// no projection and no camera at the hook where the march has to run.
//
// ===========================================================================
// 2. THE TEMPLATE IS Nanite::EmitDepthTargets' NON-COMPUTE BRANCH
// ===========================================================================
//
// NaniteComposition.cpp:256. UseComputeDepthExport() requires
// GRHISupportsDepthUAV && GRHISupportsExplicitHTile, both false on D3D12 PC, so
// the raster branch IS the PC path -- the engine runs this exact shape every
// frame on this hardware. Copied from it, deliberately and literally:
//
//   * its own pass, with FDepthStencilBinding(SceneDepth, ELoad,
//     FExclusiveDepthStencil::DepthWrite_StencilWrite) -- a DIFFERENT exclusive
//     state from the base pass's, on the same texture, in a later pass. That is
//     legal and it is what the engine does; the base pass's read-only binding
//     constrains the base pass and nothing after it.
//   * TStaticDepthStencilState<true, CF_DepthNearOrEqual> for the depth half.
//     NearOrEqual and not Greater: two point samples of the same step function
//     agree to within float error at a shared surface, and a strict test then
//     accepts about half of them (VoxelMarchSpike.usf's DEPTH SLACK note is the
//     same phenomenon from the other side).
//   * the stencil write, SO_Replace with the decal mask.
//
// AND ONE THING THAT COULD NOT BE COPIED. GET_STENCIL_BIT_MASK lives in
// Renderer/Private/PostProcess/SceneRenderTargets.h:41 -- PRIVATE, not
// reachable from a game module. The constant is mirrored below by hand
// (kStencilReceiveDecalMask), exactly as this project mirrors worldgen.ush
// against amplifier.h, with the engine line number beside it so the mirror can
// be re-checked in one grep. Getting it wrong does not crash: decals silently
// stop landing on terrain, which looks like a decal bug.
//
// ===========================================================================
// 3. THE FOUR THINGS THAT SILENTLY LOOK FINE
// ===========================================================================
//
// (a) DBUFFER DECALS -- AND THIS ONE COULD NOT BE DONE, WHICH IS A FINDING AND
//     NOT AN OMISSION. They are blended by the BASE PASS pixel shader and our
//     emit runs after it, so a decal on terrain would simply not exist. The
//     plan says to sample DBufferA/B/C and apply DecodeDBufferData /
//     ApplyDBufferData from DeferredDecal.ush. Two problems, and the second is
//     fatal at this seam:
//       - there is no DeferredDecal.ush in 5.8; the code is in
//         DBufferDecalShared.ush.
//       - THE TEXTURES ARE UNREACHABLE FROM A GAME MODULE HERE. They live in
//         FDBufferParameters / FDBufferTextures, both in
//         Renderer/Private/DBufferTextures.h. FSceneTextureUniformParameters
//         does not carry them, ESceneTextureSetupMode has no flag for them,
//         FXRenderingUtils exposes nothing, and this hook's signature hands us
//         render targets and scene textures and no third thing.
//     The apply is written and correct in VoxelMarch.usf behind
//     VOXEL_MARCH_DBUFFER; the permutation is not compiled and the cvar is
//     forced off, so nothing can silently sample black and call it "no decals".
//     MITIGATING, AND CHECKED: this project places no decals at all -- not one
//     UDecalComponent, UDecalActor or r.DBuffer setting anywhere in ue-project.
//     So nothing is being lost today. What is lost is the ability to add a decal
//     later without noticing it does not land on the ground.
//
// (b) VELOCITY, AND ITS TRAP. Reconstructed from the ray's float t, never from
//     the encoded depth. The previous-frame position of a STATIC world point is
//     computed with PrevPreViewTranslation, NOT the current PreViewTranslation:
//     using the current one produces a whole-screen velocity offset on exactly
//     the frames the streaming origin rebases, which are the frames TSR smears
//     worst. The target comes from FXRenderingUtils::GetSceneVelocityTexture
//     and is NEVER CLEARED -- HISM foliage, characters and the water have
//     already written theirs by the time we run. WITH ONE EXCEPTION THE PLAN
//     DOES NOT MENTION: the engine's own opaque velocity pass picks its load
//     action with HasBeenProduced(Velocity) (VelocityRendering.cpp:505, used at
//     :690), so by producing the texture here we SUPPRESS its clear, and every
//     pixel neither we nor a moving primitive writes is then left holding stale
//     allocation contents. With Nanite in the frame this never bites (Nanite's
//     EmitDepthTargets clears velocity before the base pass); without it, it
//     bites as far-field TSR smear. So the emit clears velocity itself, exactly
//     once, and only when nobody has produced it yet.
//
// (c) SV_Depth DISABLES EARLY-Z AND INVALIDATES HTILE for every tile it
//     touches, so everything downstream that reads depth pays decompression.
//     The mitigation is that the emit is dispatched INDIRECTLY over tiles the
//     march actually HIT, so empty screen -- sky, and everything the prepass
//     already occluded -- is never rasterised at all.
//
//     AND HERE IS THE TRAP IN MEASURING IT, WHICH COST NOTHING ONLY BECAUSE IT
//     WAS CAUGHT BEFORE A LEG RAN. **EmitGpuMs IS NOT THE HTILE COST.** It is
//     the cost of the emit pass itself -- rasterising the hit tiles and writing
//     depth, stencil and seven targets. The 0.2-0.4 ms the plan budgets is paid
//     by everything AFTER us that reads depth: RenderLights, SSAO, the
//     translucency pass, TSR. None of that is inside any bracket this file
//     owns, and none of it can be.
//
//     WORSE, MODE 2 STRUCTURALLY CANNOT CONTAIN IT. In mode 2 the emit writes a
//     SCRATCH depth buffer, so the real SceneDepth's HTILE is never invalidated
//     and every downstream pass reads undisturbed metadata. A mode-2 leg
//     measures the marcher honestly and measures the HTILE bill at exactly
//     zero, while looking like it measured both.
//
//     So there is a dedicated arm: voxel.March.HTileProbe. In mode 2 it points
//     the emit's DEPTH-STENCIL binding at the REAL SceneDepth while colour
//     still goes to scratch. Probe-on minus probe-off, same pose, same
//     everything else, differs only in whether the real depth buffer's HTILE
//     survived -- which is the number, isolated. It corrupts the image by
//     construction (real depth, raster colour) and is a TIMING ARM ONLY.
//
// (d) THE HZB IS BUILT BEFORE OUR DEPTH LANDS (DeferredShadingRenderer.cpp:
//     2752/2784), so Lumen screen traces and SSR accelerate against an HZB with
//     no terrain in it and will overshoot. This is the seam's one genuine
//     quality debt. It is CHEAP TO FALSIFY FIRST and that is the instruction:
//     A/B r.Lumen.ScreenProbeGather.ScreenTraces 0/1 on the CURRENT build -- if
//     it changes nothing today it cannot be lost tomorrow. No code here can
//     answer that; it needs one leg.
//
// ===========================================================================
// 4. THE PHASE GATE -- THREE STATES, ONE BINARY, ONE SESSION
// ===========================================================================
//
// Same doctrine as voxel.GPU.MeshDirectToPool: the control and the change ship
// in the same binary so an A/B is a cvar flip and not a rebuild.
//
//   voxel.March 0   OFF. No pass is added, no texture is allocated, no query is
//                   issued. The frame is BYTE-IDENTICAL to a build without this
//                   file. This is the control and it is the default.
//   voxel.March 1   The marcher writes to the REAL scene depth and GBuffers.
//   voxel.March 2   BOTH: the raster path keeps the real targets and the
//                   marcher writes to SCRATCH copies, so the image on screen is
//                   the control image while every marcher gate still runs.
//                   This is the mode the image and depth gates use.
//
// THE QUAD PATH IS NOT DELETED AND NOT BYPASSED. That is P4 and it is gated on
// this working. voxel.March 1 does not suppress the quads either -- suppression
// is voxel.Stream.GPUCullDebugDrawNothing 1, a knob that already exists and is
// owned elsewhere. Running voxel.March 1 WITHOUT it puts two renderings of the
// same lattice at the same depth into DepthNearOrEqual and the result is
// per-pixel noise that looks like a marcher defect and is not one; the cvar
// help says so.
//
// ===========================================================================
// 4b. voxel.March.VerifySource -- SEPARATING A TRAVERSAL BUG FROM A SOURCE BUG
// ===========================================================================
//
// PRE-REGISTERED 2026-08-19, before the brick-pool traversal exists.
//
// When the marcher swaps from FVoxelFluidOccupancy to FVoxelBrickPool, TWO
// things change at once: the data structure being walked AND the walk that
// walks it. "The depth gate still passes" cannot tell those apart, and neither
// can "source 0 still works" -- the occupancy path could keep working perfectly
// while the pool path is wrong, or the shared ray setup could regress and take
// both down together.
//
// So the control is not a separate run, it is a SAME-FRAME COMPARISON: march
// BOTH sources over the SAME rays and compare (hit, voxel, t) per pixel, inside
// the 51.2 m window where the occupancy volume has anything to say. This is the
// P3-B analogue of the plan's voxel.March.VerifyQuads and it is strictly
// stronger than either alternative, because both sides see the identical camera,
// the identical projection and the identical pixel.
//
// TWO EXCLUSIONS, BOTH NAMED IN ADVANCE, BOTH FOR DOCUMENTED REASONS:
//
// 1. MAT_WATERMARK. THE TWO SOURCES USE DIFFERENT SOLIDITY PREDICATES, and this
//    is by design, not by accident. FVoxelFluidOccupancy packs with
//    vxc::isSolidForFluid, which EXCLUDES MAT_WATERMARK (fluidoccupancy.h:132)
//    -- correct for particles. The render predicate is isSolidForRender(m) =
//    m != MAT_AIR (docs/brick-volume-format.md section 3a), which INCLUDES it.
//    So the occupancy source is blind to exactly the voxels the water-marker
//    debug instrument exists to show.
//
//    That is a NAMED EXCLUSION and not a tolerance. Format section 3a records
//    that inheriting the fluid predicate here "would delete the water-marker
//    debug instrument in exactly the mode someone enabled in order to look at
//    it" -- a silent, self-concealing failure. A comparator that quietly
//    widened a threshold to absorb it would recreate that failure one level up.
//    The pixels are identified by the POOL's material being MAT_WATERMARK,
//    counted in their own column, and excluded. If that column is non-zero the
//    log says so, because it means the marker is live at this pose and any
//    other reading of the frame has to account for it.
//
// 2. Rays whose hit lies outside the occupancy volume's 51.2 m box. The
//    occupancy source cannot have an opinion there; the pool can. Counted as
//    "pool only", never as disagreement.
//
// WHAT AGREEMENT MEANS HERE, AND WHAT IT DOES NOT. Byte-identical (hit, voxel)
// is the bar at LEVEL 0 INSIDE THE WINDOW, because both sources then describe
// the same 10 cm lattice. It is NOT the bar at coarse levels -- those sample a
// different lattice by construction and comparing them is a category error.
// t is compared under the spike's own discipline: the tie rule for adjacent
// voxels sharing a face, and the measured reference noise floor rather than
// zero. Reusing that discipline is deliberate; it took three rewrites to earn
// and its failure modes are already written down.

// ===========================================================================
// 5. THE VOLUME, AND WHAT IT IS NOT YET
// ===========================================================================
//
// FVoxelBrickPool is a concurrent workstream (P2) and did not exist when this
// was written. Until it does, the traversal source is FVoxelFluidOccupancy --
// the same 512^3 / 51.2 m bit volume VoxelMarchSpike.usf marched -- behind the
// VOXEL_MARCH_SOURCE permutation in VoxelBrickTraverse.ush, so the swap is one
// line.
//
// TWO CONSEQUENCES THAT MUST BE READ BEFORE ANY NUMBER FROM THIS IS QUOTED:
//   * the marcher can see at most ~25.6 m. Beyond that every ray leaves the box
//     and misses, and the emit writes nothing. A screenshot showing terrain
//     near and nothing far is the volume's extent, not the marcher failing.
//   * the occupancy volume has NO PER-VOXEL MATERIAL. Every solid voxel comes
//     back as MAT_ROCK. Colour A/Bs are meaningless until source 1; the DEPTH
//     gate is the one that is meaningful now, and it is the one P3 owes.
//
// ===========================================================================
// 6. HOW IT IS WIRED UP, AND WHY THAT WAY
// ===========================================================================
//
// The occupancy volume is created and owned by UVoxelFluidSubsystem (VoxelEarth
// module). Rather than reach into that subsystem or duplicate its lifetime
// rules, this module exposes ONE publisher -- VoxelMarchPublishSource -- which
// the fluid subsystem calls beside the line where it already publishes the same
// pointer to the fluid renderer's state. The publisher owns the extension's
// whole lifecycle: it creates it on first call, refreshes the volume pointer
// every tick (so an Enable-cycle re-links automatically) and is the only thing
// in the process that knows both a UWorld and a volume.
//
// That keeps this phase to ONE line inside a file this workstream does not own,
// which was the constraint.

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIGPUReadback.h" // the hit-tile count ring -- see FTileReadback
#include "SceneViewExtension.h"
#include "Templates/SharedPointer.h"

class FVoxelFluidOccupancyVolume;
class UWorld;

// ---------------------------------------------------------------------------
// The arm, asked of the cvars directly
// ---------------------------------------------------------------------------
//
// SAME RULE AS FVoxelMarchSpikeArm, and it was earned the same way: a state
// that depends on a pass running can never be used to describe whether it ran.
// "Armed but declined the frame" and "nobody asked for anything" are different
// answers and the perf line prints them as different words.
struct FVoxelMarchArm
{
	// 0 off / 1 real targets / 2 scratch targets (both).
	int32 Mode = 0;
	// voxel.March.StepBudget, clamped exactly as the pass clamps it.
	int32 StepBudget = 0;
	// voxel.March.Source: 0 = FVoxelFluidOccupancy (the Phase 0 stand-in),
	// 1 = FVoxelBrickPool (the real volume, level 0 only at P3-B1).
	int32 Source = 0;
	// voxel.March.SkipLevels: 0 flat (the control that produced the +18.3%
	// indirection number), 1 brick skipping, 2 chunk skipping as well.
	int32 SkipLevels = 0;
	// The ring cascade arm (P3-B2b-1). Source 1 only.
	bool bRings = false;
	// voxel.March.Fallthrough: how many coarser levels a ring segment may
	// retry after a miss that crossed a NON-RESIDENT chunk (Phase 1, the
	// no-hole invariant). 0 = off = the byte-identical control. Rings only --
	// forced to 0 when bRings is false, because only the ring walk reads the
	// define.
	int32 Fallthrough = 0;
	// voxel.March.HoleStats: the hole metric on the shipping kernel --
	// substituted (hit from a coarser level than the segment's owner) and
	// uncovered (no hit, crossed an ABSENT chunk, pointing below the horizon),
	// with rays and hits as denominators. A shader permutation, default off,
	// and off is free: no UAV exists, no groupshared word, no atomic. Rings
	// only, forced false without them, because both counters are properties
	// of the ring walk.
	bool bHoleStats = false;
	// The permutation value actually asked for: 0 off, 1 the cheap counters
	// (the certified 0.0302% / 8.2479% instrument, unchanged), 2 adds the
	// per-level / per-reason uncovered breakdown. bHoleStats stays the "any
	// level" summary so existing consumers keep reading one bool.
	int32 HoleStatsLevel = 0;
	// voxel.March.ReachM. 0 keeps the occupancy volume's own box, which is what
	// keeps the source A/B alive. Anything else moves the local frame and is
	// source-1 only.
	float ReachM = 0.0f;
	// voxel.March.AO, voxel.March.DBuffer, voxel.March.Velocity -- each a
	// shader permutation, each defaulting ON. They exist so that a defect in
	// one of the three "silently looks fine" items can be BISECTED without a
	// rebuild, which is the whole reason they are permutations and not
	// uniforms: a uniform branch leaves the other side's texture loads in the
	// binary and the bisection then measures the wrong thing.
	bool bAO = true;
	bool bDBuffer = true;
	bool bVelocity = true;
	// voxel.March.BlockSkip: the coarse occupancy level above the chunk index
	// (VOXEL_MARCH_BLOCK_SKIP). A PERMUTATION and not a uniform -- the same rule
	// Rings / Fallthrough / HoleStats follow and for the harder version of the
	// reason: this arm adds TWO BUFFER LOADS PER BLOCK, so a runtime branch
	// would leave that traffic in the control's binary and flatter the arm under
	// test. Rings only, forced false without them, because it is the ring walk's
	// chunk loop that consults it.
	bool bBlockSkip = false;
	// THE PER-RAY RESIDENT-EXTENT BOUND (voxel.March.Bound, Stage 0b). A
	// PERMUTATION, not a uniform -- the consumer adds up to seven texture
	// loads per ray, which is BlockSkip's argument, not ZCut's. Rings only
	// (the clamp lives in the ring walk's socket), forced 0 without them;
	// also forced 0 while voxel.March.SkyLadder is armed, because the
	// Bound+SkyLadder permutation is refused at compile (the ladder's gate
	// reads bWalkTruncated, which the bound eliminates -- see
	// VoxelMarchBound.ush's #error) and an arm must never ask for a kernel
	// that does not exist. The render site additionally forces it off under
	// voxel.March.HalfRes, the third refused pairing, because ResShift is
	// resolved per frame on the render thread and not visible here.
	int32 Bound = 0;
	// THE TIGHT RESIDENT-Z GATE (voxel.March.ZTight, the repaired ZCut). A
	// PERMUTATION, default 0 -- byte-identical control, argued at the define
	// in VoxelBrickTraverse.ush (what it removes is segment ENTRY, so a
	// runtime branch would re-base the very term under test). Rings only
	// (the gate lives at the top of the ring walk's rung body), forced 0
	// without them. WINS THE CONFLICT WITH voxel.March.Bound: the pairing is
	// refused at compile (two clamp arms folding into one absent-chunk debt
	// are un-attributable, and the bound is retired per
	// docs/marcher-cost-autopsy-2026-08-29.md), and VoxelMarchGetArm forces
	// Bound off with a logged warning when both are asked for.
	int32 ZTight = 0;
	// THE RETRY-RUNG RESIDENCY PROBE (voxel.March.RungProbe). A PERMUTATION,
	// default 0 -- byte-identical control, argued at the define in
	// VoxelBrickTraverse.ush (the term under test is the rung prologue, the
	// ZTight rule). Requires rings AND a fallthrough depth (a ladder with no
	// retry rungs offers the probe nothing to gate), forced 0 without them;
	// forced 0 while voxel.March.SkyLadder is armed, because that pairing is
	// refused at compile (the sky gate reads per-chunk sky marks a block
	// occupancy bit cannot reproduce for a rung never walked -- the #error in
	// VoxelBrickTraverse.ush) and an arm must never ask for a kernel that
	// does not exist. DELIBERATELY NOT REFUSED against Bound, ZTight or
	// HalfRes -- different mechanism, no shared state, no producer; the cvar
	// text carries the reasoning.
	int32 RungProbe = 0;
	// THE RETRY LADDER GATE (voxel.March.SkyLadder). A permutation, unlike
	// BlockSkyMode below: it puts a field on the chunk cache and a fold at five
	// walk sites, so the control must not carry it. Forced false without rings
	// and without a fallthrough depth, matching the refused permutation -- there
	// is no ladder to gate without one.
	bool bSkyLadder = false;
	// THE SKY LICENCE (voxel.March.BlockSkySkip), a UNIFORM inside the
	// VOXEL_MARCH_BLOCK_SKIP permutation and NOT a permutation of its own.
	//
	// ARGUED, NOT ASSUMED, because the doctrine every other arm here follows is
	// the opposite. That doctrine exists so a control does not carry the
	// treatment's LOADS. This mode selects between two predicates over bits the
	// block consult is already loading, and the one buffer it adds
	// (MarchBlockAllSky) is read under `if (Mode != 0)` -- a uniform branch,
	// coherent across the entire dispatch, so mode 0 issues no sky load at all
	// and the residency-only arm keeps EXACTLY the traffic its published
	// -30% result was measured with. There is no traffic for an off-arm to
	// carry, which is the same test voxel.March.ZCut passed to be a uniform.
	//
	// AND IT BUYS THE THING A PERMUTATION WOULD NOT: all three states on ONE
	// binary, so off / residency-only / sky-licensed can be interleaved in one
	// leg instead of compared across three builds. That is how the -30% figure
	// this arm has to beat was NOT measured.
	//
	//   0  residency-only -- byte-identical to the shipped BlockSkip arm
	//   1  both: a block may be skipped if it is unoccupied OR provably sky
	//   2  sky-only: residency alone no longer licenses a skip, so the sky
	//      predicate is measured on its own
	int32 BlockSkyMode = 0;
};
VOXELEARTHSHADERS_API FVoxelMarchArm VoxelMarchGetArm();

// ---------------------------------------------------------------------------
// The hole metric (voxel.March.HoleStats)
// ---------------------------------------------------------------------------
//
// THE ONE AUTHORITY FOR THE STATS BUFFER LAYOUT. The shader's word indices are
// pushed from THIS enum by FVoxelMarchCS::ModifyCompilationEnvironment --
// never restated in the .usf -- because a hand-mirrored stats layout already
// bit this project once: the index probe's "= 60; // mirrors
// VOXEL_MARCH_VIDX_WORDS" against a shader that computed 64, whose
// out-of-range writes D3D12 silently discarded, producing PLAUSIBLE WRONG
// VALUES rather than an error. One side only, pushed at compile.
// ---------------------------------------------------------------------------
// The march frame's VERTICAL budget (2026-09-02, the altitude fix)
// ---------------------------------------------------------------------------
//
// TWO CONSTANTS, TWO CLAIMS, and they meet in the frame-extent derivation in
// VoxelMarchRenderer.cpp (search FrameExtentZUU):
//
// kVoxelMarchAltitudeReachVoxels -- how far the march CUBE reaches in +/-Z
// from the snapped camera, in level-0 voxels. The owner flies to ~10 km and
// looks straight down; the old cube reused the XY reach (~8,601.6 m half
// extent), so from high poses the ground at Z=0 sat OUTSIDE the frame and the
// slab test deleted it before any walk ran -- the plan-view 'topographic rim'
// at altitude. 131,072 voxels = 13,107.2 m: covers 10 km with 3.1 km of
// margin, and is a power of two so the snap arithmetic stays exact. Z ONLY:
// the XY reach is the ring cascade's job and is untouched.
//
// kVoxelMarchPackZReachVoxels -- how far from the camera a single RUNG may
// walk vertically, in voxels OF THE RUNG'S OWN LEVEL. This is the per-level
// ceiling the VisBuffer pack and float32 both impose, and one number serves
// both claims:
//   * the pack: LocalVoxel is CAMERA-relative at the hit's own level in a
//     biased 13-bit field (-4096..+4095 -- VoxelMarchPackVis). The ring radii
//     bound XY to 1,280 level-L voxels; NOTHING bounded Z, so a fine-level
//     hit deeper than ~409.6 * 2^L m below the camera wrapped to a
//     legal-looking coordinate. 4088 leaves an 8-voxel margin for the floor()
//     at the hit and the walk's one-cell overshoot.
//   * float32: walk positions are frame-relative in level-L units, so this
//     clamp bounds them to ~40,880 walk-UU where one ulp is 0.0049 UU --
//     under half the 0.01 UU advance nudge, and 1/1000 of half a voxel. An
//     UNCLAMPED level-0 walk to the new 13.5 km cube floor would sit at
//     ~1.35M walk-UU, ulp 0.16 UU, and the nudge would round away entirely
//     (the stall/skip failure the centred-frame note in VoxelMarch.usf's
//     BuildRay documents at 409,600 UU, arriving vertically).
// The shader's copy is VOXEL_MARCH_PACK_ZREACH_VOXELS in
// VoxelBrickTraverse.ush; the two are cross-referenced by name because a
// define cannot be static_asserted across languages. Drift is bounded by the
// 8-voxel margin, but keep them equal.
//
// Ground deeper than one rung's reach is NOT lost: the retry ladder re-enters
// the segment's unwalked tail at the next coarser level (ladder-on-truncation,
// VoxelBrickTraverse.ush), and each level doubles the vertical reach -- the Z
// twin of the XY ring cascade. The deepest packable depth at the top level
// (4088 * 2^7 voxels = 52.3 km at 8 rings) is what the frame derivation clamps
// kVoxelMarchAltitudeReachVoxels against, so no cube this code can build
// outruns the ladder that has to cover it.
constexpr int32 kVoxelMarchAltitudeReachVoxels = 131072;
constexpr int32 kVoxelMarchPackZReachVoxels = 4088;
static_assert(kVoxelMarchAltitudeReachVoxels * 10 >= 10000 * 100,
              "the altitude budget must cover the owner's 10 km pose: a camera at 10 km "
              "looking straight down must still march terrain at Z=0.");
static_assert(kVoxelMarchPackZReachVoxels <= 4096 - 8,
              "the per-rung vertical reach must leave margin inside the VisBuffer's "
              "biased 13-bit field (-4096..+4095): the hit's floor() and the walk's "
              "one-cell overshoot both land past the clamp point.");
namespace VoxelMarchHoleWord
{
	enum
	{
		Rays = 0,        // rays that actually walked the volume (the denominator)
		Hits,            // rays that hit at any level (substituted's denominator)
		Substituted,     // hits from a level coarser than the owning segment
		Uncovered,       // misses that crossed an ABSENT chunk, below horizon

		// ---- LEVEL 2 (voxel.March.HoleStats 2): the uncovered breakdown ----
		// UncoveredShell is Uncovered NARROWED TO ACTUAL HOLES: misses that
		// crossed an absent chunk FACE-ADJACENT TO A RESIDENT ONE, i.e. a gap
		// in ground the streaming system holds rather than the air outside the
		// shell. Uncovered stays exactly as it was so the two are comparable
		// window for window; UncoveredShell / Uncovered is the share of the
		// old number that was ever about holes, and on the three zcut captures
		// of 2026-08-23 the old number was 25.27% of ALL RAYS on a settled
		// stationary world. Written only by the level-2 permutation, and it is
		// the denominator the attributed identity is checked against.
		UncoveredShell,
		// THE FALLTHROUGH LADDER, considered vs taken, added 2026-08-25 with the
		// shell gate (VoxelBrickTraverse.ush's VOXEL_MARCH_FALLTHROUGH_SHELL).
		//
		// WHY BOTH, AND WHY NEITHER ALONE WILL DO. `substituted` already counts
		// retries that WON A HIT -- it is a numerator with no denominator, so a
		// change that halves the retries and a change that halves their hit rate
		// read identically through it. Considered is every rung the ladder was
		// offered; Taken is every rung it actually walked. Taken/Considered is the
		// retry rate, and it is the ONLY reading that can show the shell gate is
		// doing something rather than merely compiled in.
		//
		// PLACED HERE, between UncoveredShell and the level words, and the reason
		// is worth stating because I checked it rather than assuming: this enum has
		// NO static_assert pinning its indices. The coupling that actually matters
		// is that the shader indexes the same buffer by the same enum and the CPU
		// reads it back by name (Src[VoxelMarchHoleWord::...] in
		// VoxelMarchRenderer.cpp), so inserting a member RENUMBERS every word after
		// it on BOTH sides at once and stays consistent -- but ONLY if the shader
		// is recompiled against the new header. A stale shader cache here reads the
		// wrong word and reports plausible nonsense, which is worse than a crash.
		// If you insert here, force a shader recompile and check one known-good
		// counter before trusting any of them.
		FallthroughConsidered,
		FallthroughTaken,
		// SEVEN level words (kNumLevels -- the 8 km ring took the seventh on
		// 2026-08-23) and four reason words, both written ONLY by the level-2
		// permutation. Every attributed ray adds exactly one word from each
		// group, so sum(UncLevel*) == sum(UncReason*) ==
		// "attributed uncovered", and attributed <= UncoveredShell -- NOT
		// <= Uncovered, the two count different populations -- is an identity
		// the perf line CHECKS -- a shortfall is a capture defect in the
		// shader, printed as such, never silently absorbed. Under level 1
		// these ten words exist in the buffer (one layout, one enum, one
		// readback size) and stay zero; the CPU reports them as NOT MEASURED
		// there, because a zero that means "the code that counts this was
		// compiled out" must never be printable as "no holes of this kind"
		// (two retractions this week were exactly that shape).
		UncLevelFirst,                        // + ring level 0..7 of the miss
		// THE 8 HERE IS kNumLevels, AND THE STATIC_ASSERT BELOW THE ENUM IS WHAT
		// KEEPS IT SO. It cannot reference kNumLevels directly -- that constant is
		// declared after this enum, because the enum is what sizes it -- so the two
		// spellings are checked instead of trusted. Getting this wrong does not
		// fail to compile: the level words simply run one past their group and
		// overwrite the first reason word, and every counter stays healthy while
		// the miss attribution silently reads garbage.
		UncReasonFirst = UncLevelFirst + 8,   // + VOXEL_MARCH_MISS_* 0..3

		// ---- THE RESIDENT-Z BOUND'S ENGAGEMENT TRIO (voxel.March.ZCut) -----
		// APPENDED AFTER EVERY EXISTING WORD, ON PURPOSE. The note on
		// FallthroughConsidered above says inserting RENUMBERS both sides at
		// once and is only safe with a forced shader recompile; appending
		// renumbers nothing, so a stale shader cache still reads every OLD
		// word correctly and simply leaves these three at zero -- which the
		// reader has to word as "not measured" in any case.
		//
		// THREE WORDS BECAUSE ONE CANNOT ANSWER THE QUESTION. `Consulted` is
		// every (segment, walk-level) pair where the arm was on AND that level
		// had a usable bound, i.e. every decision the arm was actually asked
		// to make -- it is the DENOMINATOR, and Consulted == 0 while the cvar
		// reads 1 is the arm being INERT, which is this project's house
		// failure and must never be spellable as "it cut nothing".
		// `Skipped` is the pairs whose interval came back EMPTY (the walk
		// never ran at all). `Clipped` is the pairs the cut NARROWED without
		// emptying -- the near-horizon case, where the ray does reach resident
		// ground but leaves the slab long before the ring ends. Skipped alone
		// would under-report the arm on exactly those rays, which is why there
		// are two numerators and not one.
		//
		// THE THREE PARTITION THE DECISION EXACTLY:
		//     Consulted - Skipped - Clipped = pairs the cut left ALONE.
		// So "armed, consulted, and changed nothing" is a readable, separate
		// finding rather than an inference from a silence.
		//
		// A SUMMED CUT *LENGTH* WAS THE OBVIOUS FOURTH WORD AND IT IS
		// DELIBERATELY ABSENT. At 1.3M rays and up to seven segments each, a
		// per-frame sum of removed ray length in UU reaches ~1e12 and WRAPS A
		// uint32 SILENTLY -- a counter that reads small precisely when the arm
		// worked hardest, which is the counter-that-means-the-opposite-of-its
		// -label failure this project has already paid for.
		//
		// THESE THREE WORDS PROVE ENGAGEMENT. THEY DO NOT ESTIMATE THE SAVING,
		// AND USING THEM THAT WAY IS A KNOWN WAY TO BE WRONG. Iteration counts
		// and wall time move in OPPOSITE directions often enough that three
		// independent groups have published it: VoxelRT measured iterations
		// -60% while traversal time went +10-15%, and Aila & Laine record
		// "~20% slower code gives same measured perf". A skipped/consulted
		// ratio is a statement about DECISIONS, not about nanoseconds -- the
		// work removed was scattered loads whose cost depends on cache state
		// and occupancy, not on how many of them there were.
		//
		// THE SAVING IS `VoxelMarch.March` FROM ProfileGPU AND NOTHING ELSE.
		// A leg where skipped is large and that number does not move is a
		// REAL NULL RESULT, not a broken counter.
		ZCutConsulted = UncReasonFirst + 4,
		ZCutSkipped,
		ZCutClipped,

		// ---- THE COARSE OCCUPANCY LEVEL'S ENGAGEMENT TRIO -----------------
		//                                        (voxel.March.BlockSkip)
		// APPENDED, for the reason the Z bound's trio was: appending renumbers
		// nothing, so a stale shader cache still reads every older word
		// correctly and leaves these three at zero -- which a reader has to
		// word as "not measured" anyway.
		//
		// THREE WORDS, PARTITIONING THE DECISION, and one of them is a SCALE
		// rather than a count:
		//   BlockConsulted  4^3 blocks the walk actually loaded a bit for --
		//                   every decision the coarse level was asked to make.
		//                   THE DENOMINATOR. Zero while voxel.March.BlockSkip
		//                   reads 1 is the arm ARMED AND INERT, which is this
		//                   project's house failure and must never be
		//                   spellable as "there was nothing to skip".
		//   BlockSkipped    blocks that came back with no resident chunk in
		//                   them AND whose exit jump beat the ordinary
		//                   one-chunk advance, i.e. skips actually TAKEN. A
		//                   block can be consulted, read empty, and still not
		//                   be skipped -- see the shader's progress guard --
		//                   and that case has to stay visible rather than
		//                   inflating the skip rate.
		//   BlockCellsAvoided  chunk cells the jumps spanned, measured as the
		//                   JUMPED RAY LENGTH divided by the chunk size at that
		//                   level. This is the number the change exists to
		//                   move, and it is a LOWER bound: a diagonal DDA
		//                   visits up to sqrt(3) times the axis count over the
		//                   same span.
		//                   DELIBERATELY NOT 64 x BlockSkipped, and both
		//                   reasons are load-bearing. That figure would carry
		//                   NO information beyond BlockSkipped itself, and at
		//                   1.3M rays it would reach ~8e9 in a frame and WRAP A
		//                   uint32 SILENTLY -- a counter that reads small
		//                   precisely when the arm worked hardest, which is the
		//                   failure the Z bound's group refused a fourth word
		//                   over.
		//
		// THESE ARE DECISIONS, NOT NANOSECONDS, AND USING THEM AS NANOSECONDS
		// IS A PUBLISHED WAY TO BE WRONG. Iteration counts and wall time move
		// in OPPOSITE directions often enough that three independent groups
		// have measured it -- VoxelRT recorded iterations -60% against
		// traversal time +10-15%, and Aila & Laine record "~20% slower code
		// gives same measured perf". THE SAVING IS `VoxelMarch.March` FROM
		// ProfileGPU AND NOTHING ELSE. A leg where BlockCellsAvoided is
		// enormous and that number does not move is a REAL NULL RESULT, not a
		// broken counter.
		BlockConsulted,
		BlockSkipped,
		BlockCellsAvoided,

		// ---- THE SKY LICENCE (voxel.March.BlockSkySkip) -------------------
		// APPENDED, for the reason both earlier groups were: appending
		// renumbers nothing, so a stale shader cache reads every older word
		// correctly and leaves these three at zero -- which a reader has to
		// word as NOT MEASURED anyway.
		//
		// WHY THREE MORE WORDS RATHER THAN READING BlockSkipped. BlockSkipped
		// counts blocks skipped WHATEVER LICENSED THEM, so with both
		// predicates on it cannot say which one paid. These three describe the
		// sky predicate alone.
		//
		//   BlockSkyLicensed  blocks whose consult found the all-sky bit SET.
		//                     Its denominator is BlockConsulted, already
		//                     counted, so the rate is licensed/consulted --
		//                     "how much of the space this ray crossed is
		//                     PROVABLY empty". Zero while the cvar reads
		//                     non-zero is the arm ARMED AND INERT.
		//
		//   BlockSkyRunBlocks / BlockSkyRuns  THE NUMBER THIS WHOLE PROGRAMME
		//                     TURNS ON, and the reason it is a pair rather
		//                     than a mean computed in the shader: a mean
		//                     cannot be summed across rays, and an atomic per
		//                     ray of a float is not available here. A RUN is a
		//                     maximal sequence of CONSECUTIVE sky-licensed
		//                     blocks along one ray; the mean is
		//                     RunBlocks / Runs on the CPU.
		//
		//                     WHY IT DECIDES: three previous skip arms died
		//                     because there was nothing long enough to
		//                     amortise a test over -- the residency-only block
		//                     arm paid 23.7 tests to avoid 11.2 cells. Here
		//                     one licensed block IS a 4-chunk advance off one
		//                     load, so the break-even is a run of 1. The run
		//                     length says how far past break-even this gets,
		//                     and a mean near 1.0 with a high licensed rate
		//                     means sky is SCATTERED -- which would say the
		//                     4^3 granularity is wrong, not that the mark is.
		//
		// A SUMMED SKIPPED *DISTANCE* IS DELIBERATELY ABSENT, for the reason
		// the Z bound's fourth word is: at 1.3M rays it reaches ~1e12 and
		// wraps a uint32 silently -- a counter that reads small precisely when
		// the arm worked hardest.
		//
		// AND THESE PROVE ENGAGEMENT, NOT SAVING. Decisions, not nanoseconds.
		// The saving is VoxelMarch.March from ProfileGPU and nothing else; a
		// leg where the licensed rate is large and that number does not move
		// is a REAL NULL RESULT, and the third one this project has had here.
		BlockSkyLicensed,
		BlockSkyRunBlocks,
		BlockSkyRuns,

		// ---- THE CHUNK-CELL LOOKUP CENSUS (2026-08-26) --------------------
		// APPENDED, for the reason every group since the Z bound has been:
		// appending renumbers nothing, so a stale shader cache still reads
		// every older word correctly and simply leaves these eight at zero --
		// which a reader has to word as NOT MEASURED anyway.
		//
		// WHAT THIS MEASURES AND WHY IT IS EIGHT WORDS AND NOT ONE. Every
		// return site of VoxelMarchLookupChunk (VoxelBrickTraverse.ush), so
		// the census PARTITIONS the function: the seven outcome words sum
		// EXACTLY to CellsProbed, by construction, and the CPU prints that
		// identity as a PASS/FAIL on the log line. That check is the whole
		// point of the shape -- this project has repeatedly shipped probes
		// that were armed and counted nothing, and a total that must
		// reconstruct from its parts is the one arrangement where "the
		// instrument did not run" and "the instrument ran and found zero"
		// cannot be spelled the same way.
		//
		//   CellsProbed        every call. THE DENOMINATOR.
		//   CellNonResident    the index dword's resident bit is clear. The
		//                      cheap early-out; one dword, no record.
		//   CellIndexEmpty     resident, and the index dword's ANYSOLID bit is
		//                      CLEAR. EXPECTED TO BE EXACTLY ZERO TODAY and
		//                      that is not a defect: all three writers of the
		//                      entry hardcode the bit set
		//                      (VoxelMarchChunkIndex.cpp:867 and :1362,
		//                      VoxelMarchIndexScatter.usf:184), so the branch
		//                      that reads it has never fired. It is counted
		//                      ANYWAY, because a hardcoded bit is exactly the
		//                      kind of thing that quietly becomes real, and
		//                      because a zero this word cannot produce would
		//                      break the identity instead of hiding.
		//   CellSlotReject     slot out of the pool's bounds, or the record
		//                      stride cross-check failed. Both are corruption
		//                      guards; both return before the record fetch.
		//   CellStale          THE RECORD WAS FETCHED (4 dwords out of the
		//                      ~15.7 MB chunk table) and its origin or ring
		//                      level disagreed with the key. A stale index.
		//   CellResidentEmpty  THE RECORD WAS FETCHED and says the chunk is
		//                      resident and ENTIRELY AIR. This is the word the
		//                      census exists for: every one of these paid a
		//                      scattered ~16 B fetch to learn something the
		//                      index dword could have carried in a bit that is
		//                      already there.
		//   CellBrickOOB       the record validated and its brick base fell
		//                      outside the descriptor pool. Also post-fetch,
		//                      so it belongs on the paying side of the ratio;
		//                      counted separately rather than folded into
		//                      CellSlotReject precisely because folding it
		//                      would move a post-fetch outcome into a
		//                      pre-fetch bucket and corrupt the denominator.
		//   CellReal           a full, valid chunk. The work the fetch is for.
		//
		// THE RATIO THE CENSUS COMPUTES is
		//     CellResidentEmpty / (CellStale + CellResidentEmpty +
		//                          CellBrickOOB + CellReal)
		// -- of the lookups that PAY FOR THE RECORD FETCH, the share thrown
		// away as air. It is a count of DECISIONS, not of nanoseconds, and the
		// same warning every group above carries applies unchanged: the saving
		// would be VoxelMarch.March from ProfileGPU and nothing else.
		CellsProbed,
		CellNonResident,
		CellIndexEmpty,
		CellSlotReject,
		CellStale,
		CellResidentEmpty,
		CellBrickOOB,
		CellReal,

		// ---- THE TERRAIN HEIGHT PYRAMID'S ENGAGEMENT GROUP ----------------
		//                                    (voxel.March.HeightPyramid)
		// APPENDED, for the reason the Z bound's trio and the block group were:
		// appending renumbers nothing, so a stale shader cache still reads every
		// older word correctly and leaves these at zero -- which a reader has to
		// word as "not measured" anyway.
		//
		//   HeightConsulted  rays that ran the pyramid DDA at all. THE
		//                    DENOMINATOR. Zero while voxel.March.HeightPyramid
		//                    reads 1 is the arm ARMED AND INERT, which is this
		//                    project's house failure and is printed as a warning
		//                    rather than left to be inferred from a silence.
		//   HeightAdvanced   rays whose walked interval STRICTLY SHRANK. A ray
		//                    can consult the field, find its first cell already
		//                    a candidate, and walk exactly what the control
		//                    walked -- that case must stay visible rather than
		//                    inflating the engagement rate.
		//   HeightEmpty      rays the pyramid proved empty over their WHOLE
		//                    interval, so no voxel walk ran at all. This is the
		//                    up-direction win and it is counted separately
		//                    because a leg can move it a great deal while
		//                    leaving the horizon untouched -- which is a PARTIAL
		//                    result and has to be reportable as one.
		//   HeightLeafCells  skipped ray length divided by the leaf cell size,
		//                    i.e. leaf cells the walk did not enter. A LOWER
		//                    bound, and deliberately not a count of cells the
		//                    DDA stepped over: the same choice, for the same
		//                    two reasons, that BlockCellsAvoided documents --
		//                    it would carry no information beyond HeightAdvanced
		//                    and it would wrap a uint32 at this ray count.
		//   HeightSteps      DDA steps taken across all levels. THE COST SIDE.
		//                    Without it a null result cannot distinguish "the
		//                    field skipped nothing" from "the field skipped a
		//                    lot and the walk cost as much as it saved".
		//   HeightReentries  extra voxel-walk calls beyond the first, i.e. the
		//                    mid-ray skips actually taken. Zero here with
		//                    HeightAdvanced large means the arm is doing tStart
		//                    only and the maximum-mipmap traversal is NOT what
		//                    was measured.
		//
		//   ---- THE FALSIFIER, AND WHY IT IS TWO WORDS AND NOT ONE ----------
		//
		//   It WAS one word, heightViolations, and that word could not decide
		//   anything. It ORed together two findings with different
		//   consequences, and a clean leg returned 4,418 of them with no way to
		//   say which kind they were. The name was honest; the AGGREGATION was
		//   not, which is a separate failure mode from this project's list of
		//   counter names that lie and deserves its own entry.
		//
		//   HeightMissed     the clamped walk found NOTHING where the full
		//                    unclamped walk found geometry. UNAMBIGUOUS HOLE.
		//                    Any non-zero here and the arm is dead as designed:
		//                    it is proven air over real ground, which is the
		//                    one failure this whole feature is written to make
		//                    impossible.
		//   HeightLate       the clamped walk hit, but more than one voxel
		//                    FURTHER along than the control. Could be ring
		//                    substitution from restarting the walk mid-ray --
		//                    a coarser level answering a segment the control
		//                    answered finely -- or could be a hole. On its own
		//                    it decides nothing, which is exactly why the next
		//                    six words exist.
		//   HeightLateMaxUU  the largest late delta seen, in UU. READ WITH
		//                    InterlockedMax AND ACCUMULATED WITH Max, not with
		//                    +=. Summing a maximum across frames produces a
		//                    number that grows with frame count and means
		//                    nothing.
		//   HeightLateB0..B4 the delta's distribution, decade buckets from the
		//                    10 UU tolerance upward:
		//                      B0  0.1 .. 1 m     B1  1 .. 10 m
		//                      B2  10 .. 100 m    B3  100 .. 1,000 m
		//                      B4  over 1,000 m
		//                    THE SHAPE IS THE VERDICT. Deltas piled into B0
		//                    just past the tolerance are float and substitution
		//                    noise; weight out in B2..B4 is real geometry being
		//                    skipped, and no count of violations alone can tell
		//                    those apart.
		HeightConsulted,
		HeightAdvanced,
		HeightEmpty,
		HeightLeafCells,
		HeightSteps,
		HeightReentries,
		HeightMissed,
		HeightLate,
		HeightLateMaxUU,
		HeightLateB0,
		HeightLateB1,
		HeightLateB2,
		HeightLateB3,
		HeightLateB4,
		// NOTE 2026-09-02: a hits-by-ring-level word group (8 words) was
		// appended here and REVERTED within hours on a wrong bracket -- the
		// vista kill predated it (two zero-diseases were being bisected as
		// one; see docs/cascade-cut-to-8-2026-09-02.md). The append pattern
		// itself was EXONERATED by that revert: probe counts were equal to
		// the digit with and without it.
		//
		// PER-GRID-SLOT STALE/REAL (2026-09-02, the vista-kill trace's
		// discriminator): which index grid slot a fetched record's
		// validation verdict belongs to. Stale concentrated in slots 6/7
		// with 0-5 clean = the R6/R7 lattice/level kill; stale spread across
		// all slots = the frame origin is wrong at that camera Z. One leg
		// decides. 12 words per group (kGridSlots-wide, cover included).
		StaleGridFirst,
		StaleGridLast = StaleGridFirst + 11,
		RealGridFirst,
		RealGridLast = RealGridFirst + 11,

		// ---- THE Z LADDER (the altitude fix, 2026-09-02) ------------------
		// APPENDED, the rule every group since the Z bound has followed:
		// appending renumbers nothing, a stale shader cache still reads every
		// older word correctly and leaves these four at zero -- which a reader
		// must word as NOT MEASURED anyway. (The RayBound census words that
		// live PAST Count in VoxelMarchRenderer.cpp DO shift with this append
		// -- both sides move together through the defines, but a stale cache
		// misreads that group: force a shader recompile before trusting it.)
		//
		//   ZLadderRungs    every rung the ring walk entered with the vertical
		//                   reach clamp compiled in (rings with
		//                   VOXEL_MARCH_FALLTHROUGH > 0). THE DENOMINATOR.
		//                   Zero while the ladder is armed is the clamp being
		//                   INERT, the house failure.
		//   ZLadderClamped  rungs whose walk interval was shortened by the
		//                   per-level vertical reach (pack field + float32 --
		//                   see kVoxelMarchPackZReachVoxels above). Rises with
		//                   camera altitude; ~zero at ground poses. A counter
		//                   that cannot fall is not a counter: this one must
		//                   read ~0 at the vista ground pose.
		//   ZLadderResumed  rungs ENTERED as a structural continuation -- the
		//                   previous rung stopped early (vertical clamp, the
		//                   512-chunk cap, or step-budget exhaustion) and the
		//                   ladder re-entered the segment's unwalked tail one
		//                   level coarser. The ladder-on-truncation arm's
		//                   proof of traffic: zero at a high-altitude down
		//                   pose means the fix is INERT there.
		//   ZLadderTailLost rays (0/1 per ray, the CapRays spelling) whose
		//                   segment still had an unwalked tail when the ladder
		//                   ran out of levels. THE FAILURE COUNTER: after this
		//                   change it must read 0 at every pose inside the
		//                   frame's Z budget; non-zero is a hole the image
		//                   gate should also be showing.
		ZLadderRungs,
		ZLadderClamped,
		ZLadderResumed,
		ZLadderTailLost,
		Count
	};
	// 8 since the cascade was cut back to 8 rings (2026-09-02; it was 11 for
	// the 65 km experiment). Both sides move together because the shader gets
	// these indices as defines from THIS enum (see the "one enum" note in
	// VoxelMarch.usf) -- there is no hand mirror to update.
	constexpr int32 kNumLevels = 8;
	// The one hand-written literal in the layout, checked rather than trusted.
	static_assert(UncReasonFirst - UncLevelFirst == kNumLevels,
	              "the level-word group must be exactly kNumLevels wide -- if it is not, the "
	              "level words overrun into the reason words and miss attribution is silently "
	              "wrong while every counter still looks healthy.");
	constexpr int32 kNumReasons = 4;
	// The reason order, mirrored from VOXEL_MARCH_MISS_* in
	// VoxelBrickTraverse.ush (never / pending / evicted / unattributed) --
	// the bucket CODES are word offsets, so this order is layout.
}

// What one 5 s perf-log window reads: sums over every frame whose readback
// landed since the last call, plus how many frames that was, so the line can
// print honest per-frame rates instead of whichever frame finished last.
// Same ran-flag discipline as FVoxelMarchStats: Frames == 0 with bArmed true
// is "on but nothing landed yet", which must never print as a healthy zero.
struct FVoxelMarchHoleStats
{
	uint64 Rays = 0;
	uint64 Hits = 0;
	uint64 Substituted = 0;
	uint64 Uncovered = 0;
	// The fallthrough ladder. Taken/Considered is the retry RATE -- the only
	// reading that separates "the shell gate cut the retries" from "the shell
	// gate is compiled in and inert". Substituted alone cannot: it counts
	// retries that WON, so a halved retry count and a halved win rate look
	// identical through it.
	uint64 FallthroughConsidered = 0;
	uint64 FallthroughTaken = 0;
	uint64 Frames = 0;   // readbacks landed in the window
	bool bArmed = false; // voxel.March.HoleStats was on when asked

	// ---- the level-2 breakdown (voxel.March.HoleStats 2) -------------------
	// Summed ONLY from frames whose kernel ran the level-2 permutation
	// (BreakdownFrames counts those). Frames > 0 with BreakdownFrames == 0
	// means the cheap counters measured and the breakdown DID NOT -- the
	// consumer must print "not measured", never these zeros as data.
	// Sized by the enum's constant, not a literal: when kNumLevels went 6 -> 7
	// (level-6 ring, 2026-08-23) a literal 6 here would have let the fold-in
	// loop write ByLevel[6] into UncoveredByReason[0] -- silent counter
	// corruption, no bounds error.
	uint64 UncoveredByLevel[VoxelMarchHoleWord::kNumLevels] = {};
	// Per-grid-slot fetch verdicts (2026-09-02) -- see the enum's note.
	uint64 StaleByGrid[12] = {};
	uint64 RealByGrid[12] = {};
	// Order is the shader's VOXEL_MARCH_MISS_* codes: [0] never admitted,
	// [1] admitted-pending, [2] evicted, [3] unattributed (the instrument
	// refused to guess -- stale resident record or reserved code).
	uint64 UncoveredByReason[4] = {};
	// Uncovered narrowed to gaps in the resident shell -- see
	// VoxelMarchHoleWord::UncoveredShell. Level-2 frames only, so it lives
	// with the breakdown and not beside Uncovered above: a zero here on a
	// level-1 window means "not measured", exactly as the histograms do.
	uint64 UncoveredShell = 0;
	uint64 BreakdownFrames = 0;
	bool bBreakdownArmed = false; // voxel.March.HoleStats >= 2 when asked

	// ---- the resident-Z bound (voxel.March.ZCut) ---------------------------
	// Counted on EVERY hole-stats level, not only level 2: the arm's
	// engagement question is "did it run", which is level-1 cheap. bZCutArmed
	// is refreshed from the CURRENT cvar at drain/peek time exactly as bArmed
	// is, so a reader can separate "off" (silence) from "on and
	// ZCutConsulted == 0" (armed and inert -- the loud case) from "on and
	// cutting". Zeros here with bZCutArmed FALSE are not a reading at all;
	// they are the arm being off.
	uint64 ZCutConsulted = 0;
	uint64 ZCutSkipped = 0;
	uint64 ZCutClipped = 0;
	bool bZCutArmed = false;      // voxel.March.ZCut was on when asked

	// ---- the terrain height pyramid (voxel.March.HeightPyramid) -----------
	// bHeightArmed IS THE CVAR, like bZCutArmed and unlike bBlockSkipArmed: this
	// arm is a uniform, not a permutation, so the cvar IS the arm. Zeros with
	// the flag FALSE are the arm being off and are not a reading at all; zeros
	// with the flag TRUE are the loud case.
	//
	// bHeightVerifyArmed is carried SEPARATELY because HeightMissed == 0 is
	// meaningless without it -- an unarmed verify cannot produce a violation and
	// a reader must not be able to mistake that for a passed falsifier.
	uint64 HeightConsulted = 0;
	uint64 HeightAdvanced = 0;
	uint64 HeightEmpty = 0;
	uint64 HeightLeafCells = 0;
	uint64 HeightSteps = 0;
	uint64 HeightReentries = 0;
	// THE FALSIFIER, SPLIT. HeightMissed is the hole; HeightLate needs its
	// distribution to mean anything. See the enum for why one word could not
	// decide this.
	uint64 HeightMissed = 0;
	uint64 HeightLate = 0;
	// A MAXIMUM, NOT A SUM. Accumulated with Max across the window -- see the
	// readback, where summing it would have produced a number that grows with
	// frame count.
	uint64 HeightLateMaxUU = 0;
	uint64 HeightLateBucket[5] = {};
	bool bHeightArmed = false;
	bool bHeightVerifyArmed = false;
	// The field's own state, stamped at bind time. A leg whose field was all
	// +INF measured NOTHING however healthy the counters look, and this is what
	// lets the log say so rather than leaving it to be inferred.
	int32 HeightLeafChunkLevel = 0;
	int32 HeightDim = 0;
	int32 HeightFilledLeaves = 0;
	int32 HeightInfiniteLeaves = 0;
	int32 HeightDeclinedLeaves = 0;
	int32 HeightNotResidentLeaves = 0;
	int32 HeightEditedLeaves = 0;
	float HeightMinUU = 0.0f;
	float HeightMaxUU = 0.0f;

	// ---- the coarse occupancy level (voxel.March.BlockSkip) ----------------
	// Counted on EVERY hole-stats level, like the Z bound's trio and for the
	// same reason: "did it run" is a level-1-cheap question.
	//
	// bBlockSkipArmed IS NOT THE CVAR AND MUST NOT BE READ AS IT. Unlike
	// voxel.March.ZCut, this arm is a PERMUTATION -- the kernel either has the
	// code in it or does not -- so the flag is refreshed from the arm actually
	// selected. Zeros with the flag FALSE are the arm being off and are not a
	// reading at all; zeros with the flag TRUE are the loud case.
	uint64 BlockConsulted = 0;
	uint64 BlockSkipped = 0;
	uint64 BlockCellsAvoided = 0;
	bool bBlockSkipArmed = false;
	// The sky licence. BlockSkyMode is the UNIFORM as the dispatch actually saw
	// it, not the cvar: a leg whose kernel was compiled without
	// VOXEL_MARCH_BLOCK_SKIP reads none of these however the cvar is set, and
	// bBlockSkipArmed beside it is what separates "armed and inert" from "not
	// compiled in". Both must be printed or a zero is unreadable.
	uint64 BlockSkyLicensed = 0;
	uint64 BlockSkyRunBlocks = 0;
	uint64 BlockSkyRuns = 0;
	int32 BlockSkyMode = 0;

	// ---- the chunk-cell lookup census --------------------------------------
	// Counted on EVERY hole-stats level, like the Z bound's and the block
	// level's groups and for the same reason: "how often does this branch
	// fire" is a level-1-cheap question and gating it behind level 2 would
	// make every level-1 leg read the census as inert.
	//
	// THE SEVEN OUTCOMES MUST SUM TO CellsProbed. The perf line computes that
	// sum and prints PASS or FAIL. A FAIL means a return path in
	// VoxelMarchLookupChunk is uncounted -- an INSTRUMENT defect -- and the
	// ratio below it must not be read at all on such a window. bCensusArmed
	// separates "the counters were compiled out" from "they ran and the world
	// had nothing to count"; zeros with the flag FALSE are not a reading.
	uint64 CellsProbed = 0;
	uint64 CellNonResident = 0;
	uint64 CellIndexEmpty = 0;
	uint64 CellSlotReject = 0;
	uint64 CellStale = 0;
	uint64 CellResidentEmpty = 0;
	uint64 CellBrickOOB = 0;
	uint64 CellReal = 0;
	bool bCensusArmed = false;

	// ---- the tight resident-Z gate (voxel.March.ZTight) --------------------
	// Counted on EVERY hole-stats level, the Z bound's rule. bZTightArmed IS
	// NOT THE CVAR (the arm is a permutation -- the block-skip rule, not the
	// ZCut one): it is refreshed from the arm actually selected, so zeros
	// with the flag FALSE are the arm being off, and zeros with it TRUE are
	// the loud armed-and-inert case. The four words partition every
	// consultation: consulted = SegSkipped + SegEntered + RetrySkipped +
	// RetryEntered, own-level and retry rungs kept apart because "the
	// segments never skip" and "the retries never skip" are different
	// diagnoses (slab too wide vs ladder opening into populated levels).
	uint64 ZTightSegSkipped = 0;
	uint64 ZTightSegEntered = 0;
	uint64 ZTightRetrySkipped = 0;
	uint64 ZTightRetryEntered = 0;
	bool bZTightArmed = false;

	// ---- the retry-rung residency probe (voxel.March.RungProbe) ------------
	// Counted on EVERY hole-stats level, the ZTight rule, and bRungProbeArmed
	// IS NOT THE CVAR for the ZTight reason (the arm is a permutation; the
	// raw cvar would read "armed" for a kernel that contains no probe --
	// fallthrough off, or the sky ladder forcing it off). RungProbed
	// partitions exactly into RungSkipped + RungWalked; RungProbeBits is the
	// COST DENOMINATOR (occupancy bits tested -- the price of the probe as a
	// reading, not an assumption). probed == 0 with the flag TRUE is the loud
	// armed-and-inert case. THE PRE-REGISTERED VERDICT (59M retries/window,
	// hit rate 0.054%, sky pose): skipped/probed >= ~95% at sky; the timing
	// gate is VoxelMarch.March at SKY AND DOWN; `substituted` MUST NOT FALL
	// vs the control and `uncovered` must not rise -- either moving refutes
	// the arm and the default stays 0.
	uint64 RungProbed = 0;
	uint64 RungSkipped = 0;
	uint64 RungWalked = 0;
	uint64 RungProbeBits = 0;
	bool bRungProbeArmed = false;

	// ---- the Z ladder (the altitude fix, 2026-09-02) -----------------------
	// Counted on EVERY hole-stats level, the ZTight rule. bZLadderArmed is
	// FROM THE ARM, the permutation rule: the vertical reach clamp and the
	// structural continuation compile only under rings with a fallthrough
	// depth, so the raw cvars would read "armed" for a kernel that contains
	// neither. Rungs == 0 with the flag TRUE is the loud armed-and-inert
	// case; TailLost > 0 on ANY pose inside the frame's Z budget is a
	// regression FINDING, not a statistic -- a segment tail no level walked.
	uint64 ZLadderRungs = 0;
	uint64 ZLadderClamped = 0;
	uint64 ZLadderResumed = 0;
	uint64 ZLadderTailLost = 0;
	bool bZLadderArmed = false;

	// ---- the wave-occupancy census (family A's falsifier) ------------------
	// Compiled with hole stats wherever the engine's wave-op macros allow
	// (VOXEL_MARCH_WAVE_CENSUS in VoxelBrickTraverse.ush); there is no
	// separate arm. Samples == 0 on a hole-stats window is NOT MEASURED
	// (wave ops unavailable, or no walk sampled), never "no divergence".
	// WaveLaneCount is a MAXIMUM (the kernel writes it with InterlockedMax,
	// the landing accumulates it with Max -- the HeightLateMaxUU spelling):
	// it is the wave width the kernel actually compiled to, and it is the
	// denominator the two means are judged against -- mean near the width =
	// divergence is NOT the problem, family A dies; low = it lives.
	uint64 WaveChunkActiveSum = 0;
	uint64 WaveChunkSamples = 0;
	uint64 WaveCellActiveSum = 0;
	uint64 WaveCellSamples = 0;
	uint64 WaveLaneCount = 0;
};

// Drains the accumulated window (the GetAndReset pattern the 5 s perf log
// already uses for FVoxelMarchChunkIndex::GetAndResetApplyDeltaMs). Safe from
// the game thread; returns zeros with bArmed=false when the extension has
// never run.
VOXELEARTHSHADERS_API FVoxelMarchHoleStats VoxelMarchGetAndResetHoleStats();

// The most recently DRAINED window, kept for the streaming HUD panel
// (voxel.Debug 3), which redraws at 1 Hz and must not race or double-drain the
// 5 s perf log -- the log drains, this peeks what the log drained. Frames == 0
// in the returned struct means no window has completed yet ("no sample", not
// "no holes"); bArmed/bBreakdownArmed are refreshed from the CURRENT arm at
// call time so the panel's off/on wording tracks the switch, not the stale
// window. Game thread.
VOXELEARTHSHADERS_API FVoxelMarchHoleStats VoxelMarchPeekLastHoleWindow();

// ---------------------------------------------------------------------------
// THE STENCIL CONSTANT, MIRRORED BY HAND
// ---------------------------------------------------------------------------
//
// Engine/Source/Runtime/Renderer/Private/PostProcess/SceneRenderTargets.h:
//     #define STENCIL_RECEIVE_DECAL_BIT_ID  7                        (line 26)
//     #define GET_STENCIL_BIT_MASK(N,V) uint8((uint8(V)&1)<<(...))   (line 41)
// so GET_STENCIL_BIT_MASK(RECEIVE_DECAL, 1) == 0x80. That header is under
// Renderer/Private and UBT does not expose it to a game module, so the value is
// restated here with its provenance rather than included.
//
// Nanite writes exactly this bit in its own emit (NaniteComposition.cpp:290,
// StencilDecalMask), which is the behaviour terrain has to match: without it,
// deferred decals stop compositing onto voxel ground and nothing errors.
inline constexpr uint8 kVoxelMarchStencilReceiveDecalMask = 0x80;

// ---------------------------------------------------------------------------
// What the renderer reports back
// ---------------------------------------------------------------------------
//
// Same ran-flag discipline the fluid renderer's stats carry, and for the same
// reason: a pass that never ran must never be able to print a small number.
// -1.0 means no GPU timing has ever landed. A zero Frames count means the pass
// was never added at all. Both are deliberately distinct from a real 0.00.
struct FVoxelMarchStats
{
	// THE TWO NUMBERS P3 OWES, bracketed separately because they sit in
	// different halves of the frame and answer different questions:
	//   MarchGpuMs  -- classify + march, at PreRenderBasePass. Compare against
	//                  the raster path's PrePass + BasePass terrain time.
	//   EmitGpuMs   -- the indirect SV_Depth/GBuffer raster, at
	//                  PostRenderBasePassDeferred. This is where the 0.2-0.4 ms
	//                  HTILE budget shows up, and it is bracketed on its own so
	//                  that budget is measured rather than assumed.
	float MarchGpuMs = -1.0f;
	float EmitGpuMs = -1.0f;

	// MODE 2 ONLY, and it exists so that a frame-time A/B against voxel.March 0
	// is not quietly wrong. Mode 2 allocates and CLEARS a full scratch copy of
	// the depth buffer and up to six colour targets -- seven full-screen clears
	// the control frame does not pay. That is real GPU time, it sits inside the
	// frame, and it is not marcher cost. Bracketed separately so it can be
	// subtracted rather than argued about.
	//
	// mode2 frame - mode0 frame  ==  MarchGpuMs + EmitGpuMs + ScratchGpuMs
	// is the identity this field makes checkable. If it does not close,
	// something is being paid that none of the three brackets contain.
	float ScratchGpuMs = -1.0f;

	// THE BOUND PRODUCER'S OWN BRACKET (voxel.March.Bound): the list pass
	// plus the per-level cube rasters, and NOTHING of the march. Its own ring
	// and its own field because an arm whose prepass hides inside marchMs
	// cannot be judged -- a leg would read the clamp's saving net of a
	// producer cost it never itemised, which is how a win gets credited to
	// the wrong half of a change. Printed on the bound engagement line in
	// VoxelMarchGetAndResetHoleStats (this workstream's own log line, beside
	// the ray-bound census's); the external perf printer does not know this
	// field yet and that is acceptable for Stage 0b.
	float BoundGpuMs = -1.0f;

	uint64 Frames = 0;          // frames the march pass was added
	uint64 EmitFrames = 0;      // frames the emit pass was added
	uint64 ScratchFrames = 0;   // frames the mode-2 scratch set was built
	uint64 BoundFrames = 0;     // frames the bound producer passes were added

	// voxel.March.HTileProbe was in force for the frame these numbers describe.
	// Carried WITH the data, not asked of the cvar at print time: the probe
	// changes what EmitGpuMs and the frame time mean, and a reading joined to
	// the wrong arm by hand is worse than no reading.
	bool bHTileProbe = false;
	int32 Mode = 0;             // the mode THIS FRAME'S passes used

	// TilesHit IS THE NUMBER THE EMIT RASTERISES. The compact pass builds the
	// indirect draw's InstanceCount from tiles the march actually hit, so
	// TilesHit / TilesTotal is the fraction of the screen that pays SV_Depth's
	// early-Z loss -- the HTILE mitigation, measured.
	//
	// THERE IS NO SEPARATE "CLASSIFIED" STAGE, and an earlier version of this
	// struct implied there was: it carried a TilesClassified field that was set
	// to TilesTotal, which read as "the classifier rejected nothing, so the
	// indirect emit is buying you nothing" -- the opposite of what is happening.
	// The march dispatches over the whole view rect (correct: the volume is
	// centred on the camera and covers the screen); the SELECTION happens after
	// it, from real hits, which is strictly better than a conservative
	// pre-classify would have been. The field is gone rather than fixed.
	uint32 TilesHit = 0;
	uint32 TilesTotal = 0;

	// The source THIS FRAME'S passes used, and how much of the world the chunk
	// index could see. Printed together: an index holding zero entries while the
	// pool holds ~87,800 resident chunks is the two halves not having met, and
	// on a black screen that is indistinguishable from an empty world.
	int32 Source = 0;
	int32 IndexEntries = 0;
	// Consecutive frames the chunk index has gone without an upload. The
	// comparator refuses to sample below voxel.March.SettleFrames, and the
	// number is published so a reader can see how close it was.
	int32 IndexQuietFrames = 0;
	// Consecutive frames the STREAMING system has reported converged
	// (jobsInFlight 0, pendingJobs 0, chunk count unchanged). -1 means the
	// publisher has never been called and the signal is unwired.
	int32 StreamingConvergedFrames = -1;

	// ---- the index/record join probe (voxel.March.VerifyIndex) -------------
	//
	// Traces no rays. Walks the chunk cells the marcher could touch and asks
	// whether the index and the record agree, which isolates the join from the
	// DDA. Built after the first brick-pool A/B came back with a populated index
	// and 14% of the occupancy source's hit tiles -- a combination that is a
	// symptom, not a cost result.
	struct FIndexProbe
	{
		bool bValid = false;
		uint32 Probed = 0;
		uint32 Resident = 0;
		uint32 SlotOutOfRange = 0;
		uint32 OriginMismatch = 0;
		uint32 LevelMismatch = 0;
		uint32 AnySolidClear = 0;
		uint32 BrickBaseOutOfRange = 0;
		uint32 Valid = 0;
		uint32 L1Any = 0;
		uint32 L1Zero = 0;
		uint32 Claimed = 0;
		// Layer 2: the same 32^3 voxels counted through both sources. The pool's
		// count should be the LARGER -- m != MAT_AIR is a strict superset of the
		// fluid predicate -- so anything else localises the defect in one read.
		uint32 PoolSolidCells = 0;
		uint32 OccSolidVoxels = 0;
		// Bricks whose L1 bit is SET, by the kind their descriptor decodes to.
		// Any count in KindAir is a contradiction: the writer sets an L1 bit only
		// for a brick with solid content.
		uint32 KindAir = 0;
		uint32 KindSolid = 0;
		uint32 KindMixed = 0;
		struct FSample
		{
			FIntVector Want = FIntVector::ZeroValue;
			FIntVector Got = FIntVector::ZeroValue;
			uint32 Slot = 0;
			uint32 LevelAndFlags = 0;
		};
		static constexpr int32 kMaxSamples = 6;
		int32 SampleCount = 0;
		FSample Samples[kMaxSamples];

		// SIXTY-FOUR, because 15 and 100 disagreements out of 1.35M is a
		// LISTABLE population. Same-frame certification needs the whole defect
		// characterised in one run, not sampled across runs that march different
		// worlds.
		static constexpr int32 kMaxHvfSamples = 64;
	};
	FIndexProbe IndexProbe;

	// ---- the source comparator (voxel.March.VerifySource) ------------------
	//
	// Both traversals, same rays, same frame. Pre-registered before the brick
	// walk existed; built once three layers below it had been eliminated by
	// measurement and the walk was the only surface left.
	struct FSourceCompare
	{
		bool bValid = false;
		uint32 Rays = 0;
		uint32 AgreeMiss = 0;
		uint32 AgreeHit = 0;
		uint32 Ties = 0;
		uint32 BrickMissed = 0;   // occupancy hit, brick did not -- THE SYMPTOM
		uint32 BrickExtra = 0;    // brick hit, occupancy did not
		uint32 Watermark = 0;     // ... and it was a watermark voxel: excluded by name
		uint32 Hard = 0;          // both hit, different voxel, not a tie
		// THE DENOMINATOR. The occupancy walk against a 1e-7-nudged copy of
		// itself, on the same rays. Phase 0 established that reading a
		// disagreement count against zero rather than against this turns float
		// ordering noise into a phantom defect.
		// The floor for hier-vs-flat: the FLAT BRICK walk against a 1e-6 UU entry
		// nudge. Same construction, same volume, same arithmetic history the
		// hierarchy's restarts perturb -- and no fluid in it.
		uint32 RefNoise = 0;
		// The floor for the OCCUPANCY comparison, which only runs when the march
		// frame is the volume frame. Separate slot: two comparisons, two floors,
		// and conflating them is how a verdict gets read against the wrong one.
		uint32 OccNoise = 0;
		// False when the occupancy half was skipped -- brick-only mode. Carried
		// so a zeroed source-compare row reads as "not run" and never as "ran
		// and found nothing wrong".
		bool bOccValid = false;
		// The conditions the sample was taken under, carried in the same readback
		// as the counts. A residency number joined to a count by hand is joined
		// to the wrong frame -- the readback lands two or three frames late.
		uint32 Residency = 0;
		uint32 Uploads = 0;
		// The world this sample marched, as a content hash of the chunk index,
		// and the pool's own level-0 count for completeness. Equal hashes mean
		// two legs marched the SAME world -- which a residency count cannot say.
		uint64 IndexHash = 0;
		uint32 PoolLevel0 = 0;
		uint32 Claimed = 0;
		// THE SKIP RATIO, on identical rays: stepsFlat / stepsHier. And the gate
		// on it -- a hierarchy that skips a diagonal cell makes this number
		// BETTER while being wrong, so the disagreement count rides beside it and
		// is read against the same noise floor.
		uint32 StepsOcc = 0;
		uint32 StepsFlat = 0;
		uint32 StepsHier = 0;
		uint32 HierVsFlat = 0;
		// Split by direction: lost = flat hit and the hierarchy missed (it
		// skipped real content); gained = the reverse; moved = both hit but
		// different voxels. Three different causes, one useless total.
		uint32 HvfLost = 0;
		uint32 HvfGained = 0;
		uint32 HvfMoved = 0;
		// THE BIMODAL SPLIT. `lost` and `moved` have been summed all night across
		// two populations with different causes: a structural brick-face defect
		// that dominates when it fires, and a small diffuse residue consistent
		// with the restart float floor. A fix should drive the on-face term to
		// zero and leave the off-face term standing beside RefNoise.
		uint32 LostOnFace = 0;
		uint32 LostOffFace = 0;
		uint32 MovedOnFace = 0;
		uint32 MovedOffFace = 0;
		// Over EVERY lost ray, from the descent MASK rather than from the last
		// descent: 0 = the hit's brick was never descended into (the advance
		// skipped past it), 1 = it was, and the inner DDA still missed
		// (a stepping defect), 2 = inconclusive, the mask is a different chunk.
		uint32 Descend[3] = {};
		// OPPORTUNITY DENOMINATORS. Six independent worlds put the raw disagree
		// count across a 695x range (15 .. 10,428), so a count certifies nothing
		// at any feasible N -- it mixes how often the defect fires with how much
		// triggering geometry the world happens to hold. Normalising by RAYS does
		// not help; the variance is in density, not population. These divide by
		// the opportunity instead: BrickEntries is the general denominator,
		// ThinClips the one the corner-clip mechanism names. Both are predictions
		// under test, not yet established as stable.
		uint64 BrickEntries = 0;
		uint64 ThinClips = 0;
		// THE COINCIDENCE TEST AND ITS CONTROL. BrickEntries/ThinClips above are
		// retained but they DID NOT WORK: measured across six states they are
		// constants of the camera pose (0.7% and 0.9%), so dividing by them was
		// arithmetically inert -- 72x per clip against 72.4x raw. A denominator
		// only normalises if it CO-VARIES with what it divides.
		//
		// These four do the test those two could not. DisThin/DisHit is the thin
		// share among disagreeing rays; HitThin/FlatHit is the same share over
		// every ray that hit. THE LIFT BETWEEN THEM IS THE RESULT -- near 1.0
		// means thin clips are simply common and the mechanism is not indicated;
		// far above 1.0 means disagreements concentrate where the mechanism says.
		uint32 FlatHit = 0;
		uint32 HierHit = 0;
		uint32 HitThin = 0;
		uint32 DisHit = 0;
		uint32 DisThin = 0;
		// The perturbation's MAGNITUDE, which is a fingerprint rather than a
		// clue -- unlike the boundary concentration, which was a selection
		// effect and could not have come out any other way. Bin k holds rays
		// whose normalised chord through the hit voxel is of order 1e-k, so
		// bins 0-1 are clean crossings (a disagreement there is a whole-voxel
		// logic error) and high bins are corner grazes. Two columns for the
		// same reason the coincidence test needed two.
		static constexpr int32 kBandBins = 12;
		uint32 BandAll[kBandBins] = {};
		uint32 BandDis[kBandBins] = {};
		// The arbiter: a DIRECT non-accumulating slab test, O(1) ulp against a
		// walk's O(N), asked per disagreeing ray. ArbHier is the one the gate
		// as built could never report -- the flat walk being wrong.
		uint32 ArbFlat = 0;
		uint32 ArbHier = 0;
		uint32 ArbTie = 0;
		uint32 ArbNeither = 0;
		// WHY the arbiter could not decide, per side: 0 ok, 1 no candidate,
		// 2 degenerate span, 3 outside segment, 4 not solid in the pool.
		// Separated because "flat's candidate failed" and "hier's candidate
		// failed" are different findings with different fixes.
		// 0 ok, 1 no candidate, 2 degenerate span, 3 before TIn, 4 after TOut,
		// 5 not solid in the pool. Segment is split BY DIRECTION: it dominates
		// two legs of three and is a real second cause, but it cannot be
		// attributed while the degenerate rejection inflates the population it
		// draws from -- so it is made readable, not fixed.
		uint32 ArbFailA[6] = {};
		uint32 ArbFailB[6] = {};
		// The probe's own reason when side A was class 4, VOXEL_MARCH_REJECT_*.
		uint32 ArbFailAReason[7] = {};
		// Ill-posed rays binned by the band metric -- the direct test of whether
		// ill-posedness is a grazing artefact of the strict slab inequality.
		uint32 ArbFailBand[kBandBins] = {};
		// Conservation: must equal lost+gained+moved or nothing on the page is
		// comparable to anything else.
		uint32 ArbTotal = 0;
		// Uncontested: only one walk offered a candidate. `lost` and `gained`
		// are not three-way contests, and scoring them as though they were is
		// what manufactured both the inflated hier-correct counts and the
		// ill-posed population (ill-posed ~= gained, leg after leg).
		uint32 ArbFlatOnly = 0;
		uint32 ArbHierOnly = 0;
		// Largest overshoot past TOut, in UU. Separates "the arbiter's own
		// precision" (8 ulp, ~5e-3 UU here) from "a walk returned a hit outside
		// the segment it was given" (voxel scale, 10 UU).
		float SegHiMaxOverUU = 0.0f;
		// ---- the ring cascade columns (P3-B2b-1) -------------------------
		// Pre-registered before the source existed and read, not adjusted.
		// Zero through every level-0 leg was the self-test; B-2b-1 is the first
		// time it reports for real.
		uint32 RingDiscordant = 0;
		// Gate v4 split by level. The 1.5x bar is a LEVEL-0 number; whether the
		// asymmetry is level-dependent is the deliverable that justified
		// building past a FAIL.
		static constexpr int32 kRingLevels = 6;
		uint32 FlatOnlyL[kRingLevels] = {};
		uint32 HierOnlyL[kRingLevels] = {};
		// Which ring the hit population actually came from. A leg that never
		// reached ring 1 cannot say anything about ring 1, and that has to be
		// visible rather than inferred from a silence.
		uint32 HitAtLevel[kRingLevels] = {};
		// Rays that spent the step budget instead of reaching a surface or the
		// segment end. Distinguishes a budget-limited leg from a traversal-
		// limited one; without it the two are the same observation.
		uint32 BudgetFlat = 0;
		uint32 BudgetHier = 0;
		// The largest step count the flat reference needed. Turns "clipped" into
		// the number required to stop clipping.
		uint32 MaxStepsFlat = 0;
		// B: where rays left before being counted. 0 out of bounds, 1 the ray
		// never met the volume, 2 the prepass depth clipped the segment away.
		uint32 EarlyOut[3] = {};
		// D: rays that OPENED ring N's interval, against HitAtLevel which counts
		// those that found something there. entered==0 is "never got there";
		// entered>0 with hit==0 is "got there and found nothing".
		uint32 EnteredLevel[kRingLevels] = {};
		uint32 Sky = 0;
		// F: provenance of the DISPATCH that produced these counts, stamped in
		// the buffer rather than sampled when the line prints -- the readback
		// lands frames later.
		uint32 FrameNumber = 0;
		uint32 QuietFrames = 0;
		// E: the GPU's view of state the CPU also knows. Two numbers that must
		// agree beat either alone; this is the join where derived-not-verified
		// has bitten repeatedly.
		uint32 GpuResidency = 0;
		uint32 GpuUploads = 0;
		// What the KERNEL was compiled with, as opposed to what the cvar asked
		// for. bit0 rings, bits1-2 skip levels, bit3 source.
		uint32 ArmStamp = 0;
		// The CPU's belief, carried alongside so the two can be compared.
		bool bArmRingsCpu = false;
		FIntPoint ViewRectSize = FIntPoint(1, 1);
		// Uncontested rays whose attributing arm did not hit. Must be 0 by
		// construction; non-zero means the per-level split is silently landing
		// everything on L0.
		uint32 Unattributed = 0;
		// WHERE the disagreeing rays are, in screen space. 16x16 tile occupancy
		// as a 256-bit mask, plus the extent. Few tiles + small box is one
		// contiguous region (a residency or upload event); many tiles + large
		// box is frame-wide scatter (timing or a race). A box alone cannot tell
		// those apart -- two corner clusters look identical to a scatter -- so
		// the mask is the discriminator and the box is the spread.
		uint32 DisTileMask[8] = {};
		FIntPoint DisBoxMin = FIntPoint::ZeroValue;
		FIntPoint DisBoxMax = FIntPoint::ZeroValue;
		// Where `moved` rays land relative to the flat walk. An off-by-one at
		// brick entry puts the hierarchy exactly one voxel FURTHER along the ray;
		// a stepping error inside a brick scatters. 0 = ahead by one, 1 = behind
		// by one, 2 = anything else.
		uint32 MovedDelta[3] = {};
		// Why the brick path rejected the voxel occupancy hit. Indexed by
		// VOXEL_MARCH_REJECT_*: 0 solid, 1 no chunk, 2 record, 3 L1 clear,
		// 4 kind air, 5 occ clear, 6 bounds. A large count in 0 means the pool
		// DOES hold that voxel as solid and the walk never tested it.
		uint32 Reasons[7] = {};
		struct FSample
		{
			FIntPoint Pixel = FIntPoint::ZeroValue;
			FIntVector OccVoxel = FIntVector::ZeroValue;
			FIntVector BrickVoxel = FIntVector::ZeroValue;
			float OccT = 0.0f;
			uint32 BrickSteps = 0;
			uint32 Reason = 0;
		};
		static constexpr int32 kMaxSamples = 6;
		int32 SampleCount = 0;
		FSample Samples[kMaxSamples];

		// SIXTY-FOUR, not six. A disagreement population of 15-1,000 rays out of
		// 1.35M is LISTABLE, and same-frame certification wants all of it rather
		// than a sample of it: six rays can show a mechanism but cannot say whether
		// it accounts for the population.
		//
		// Declared HERE and not in the struct above, which is where it first landed
		// and would not compile -- the two structs each carry their own kMaxSamples
		// and borrowing a bound across them is how an array gets sized by the wrong
		// constant without anyone noticing.
		static constexpr int32 kMaxHvfSamples = 64;

		// The hierarchy dump: which BRICK each walk's voxel belongs to. Separates
		// "descended into the right brick, stopped on the wrong cell" from "read
		// one brick's bits for a voxel in another".
		struct FHvfSample
		{
			FIntPoint Pixel = FIntPoint::ZeroValue;
			FIntVector FlatVoxel = FIntVector::ZeroValue;
			FIntVector HierVoxel = FIntVector::ZeroValue;
			float FlatT = 0.0f;
			float HierT = 0.0f;
			uint32 Class = 0;      // 0 lost, 1 gained, 2 moved
			// The `lost` half. On a miss the hierarchy has no hit voxel, so the
			// brick comparison says nothing -- these are what separate skipped,
			// stopped-short, and never-got-there.
			uint32 TermReason = 0; // 0 hit, 1 segment, 2 budget, 3 chunk cap
			uint32 HierSteps = 0;
			uint32 ProbeReason = 0;// the brick path's verdict on the flat hit voxel
			FIntVector LastChunk = FIntVector::ZeroValue;
			int32 LastBrick = -1;      // brick CONSIDERED
			int32 LastFineBrick = -1;  // brick actually WALKED
			float LastFineT = -1.0f;   // ... and the t it was entered at
			uint32 FineWalks = 0;
			// The two properties a shared mechanism would show. FaceAxis is
			// axis|sign<<2; CellInBrick is cx|cy<<3|cz<<6, each 0..7.
			uint32 FlatFace = 0;
			uint32 FlatCellInBrick = 0;
			uint32 DescendVerdict = 0;
		};
		// Over EVERY lost ray, not just the six sampled.
		uint32 LostTerm[4] = {};
		uint32 LostProbe[7] = {};
		uint32 HvfClaimed = 0;
		int32 HvfSampleCount = 0;
		FHvfSample HvfSamples[kMaxHvfSamples];
	};
	FSourceCompare SourceCompare;
	// Frames declined because the pool or its index had nothing to march. Its
	// own counter, because "the pool is empty" and "the marcher is off" are
	// different diagnoses and the existing counters cannot tell them apart.
	uint64 DeclinedNoPool = 0;

	// ---- the depth gate (voxel.March.VerifyDepth) -------------------------
	//
	// Plan section 10, gate 3. bValid is the ran-flag and is deliberately
	// distinct from an all-zero result: "the gate ran and found nothing wrong"
	// and "the gate never ran" are different answers and only one of them earns
	// P4.
	//
	// THE PRE-REGISTERED CRITERION, stated here so it cannot be renegotiated
	// after seeing the number. REVISED ONCE, on evidence, and the revision is
	// recorded rather than overwritten:
	//
	//   v1 (dead):  disagreements / compared < 0.5% AND DisagreeInterior == 0
	//               Two runs showed this cannot pass however correct the marcher
	//               is -- 99.8% of disagreements were silhouette pixels, where
	//               both renderers are point samples of a step function and are
	//               entitled to differ, and the edge mask credits only ~58% of
	//               them. A FAIL from it carried no information.
	//
	//   v2 (live):  interior disagreement rate <= the CONTROL's interior rate
	//               AND interior max < 1.0 voxel
	//               where the control is the raster depth against a
	//               one-pixel-displaced copy of itself, through the same
	//               threshold and mask. Calibrate against the reference's own
	//               noise floor, never against zero.
	//
	// ===================================================================
	// v4, 2026-08-20. THE HIER-VS-FLAT GATE, REDEFINED AGAINST THE ARBITER
	// BECAUSE ITS PREVIOUS BASIS WAS FALSIFIED
	// ===================================================================
	//
	//   OLD BASIS (DEAD): the flat brick walk is truth; hier-vs-flat
	//   disagreements must fall to zero.
	//
	//   WHY IT IS DEAD, MEASURED NOT ARGUED. A direct non-accumulating slab
	//   test (the arbiter) adjudicated the contested rays -- those where BOTH
	//   walks offered a candidate -- across four legs:
	//
	//       flat-correct 34-40%   hier-correct 35-39%   tie 14-21%
	//
	//   A coin flip with a fifth of the population genuinely undecidable. At a
	//   cell plane the answer is ambiguous in float, so NEITHER WALK IS RIGHT
	//   and "disagreements must reach zero" asks for something arithmetic
	//   cannot supply. Roughly 90% of the disagreement population is this.
	//
	//   The old gate was not too strict. It was ILL-POSED -- it compared an
	//   instrument against another instrument of the same precision and read
	//   the difference as one of them being wrong.
	//
	//   NEW BASIS (LIVE): the arbiter is the reference, and the gate is stated
	//   on the UNCONTESTED population only -- rays where exactly one walk
	//   offered a candidate at all and the arbiter confirmed that candidate is
	//   genuinely on the ray and genuinely solid in the pool.
	//
	//       flat-only   the hierarchy missed content that is really there
	//       hier-only   the FLAT walk missed content that is really there
	//
	//   AND hier-only IS THE CONTROL, not zero. Both terms are "one walk found
	//   what the other did not"; a process with no directional defect produces
	//   them in equal numbers. So the statistic is the ASYMMETRY:
	//
	//       PASS when 0.67 * hier-only <= flat-only <= 1.5 * hier-only
	//
	//   TWO-SIDED, CORRECTED 2026-08-20 AFTER IT PASSED A CATASTROPHE. As first
	//   written the bar tested only the high side, so a leg reading flat-only
	//   6,826 against hier-only 225,022 -- an 0.03x asymmetry, 284 sigma from
	//   1.0, a NEGATIVE excess of 16% of the frame -- was reported as PASS. A
	//   RATIO BOUNDED ON ONE SIDE IS A BOUND, NOT A GATE. The two directions are
	//   different failures and the verdict names which: above, the hierarchy
	//   MISSES real content (the skip defect); below, it finds content the flat
	//   walk does not, which is a broken reference or a hallucinating walk and is
	//   the harder of the two.
	//
	//   1.5 was derived as follows: a symmetric process gives 1.0, and at the
	//   count sizes involved (~10-25 per leg) Poisson noise alone is about
	//   +/-30%.
	//
	//   THAT DERIVATION WAS WRONG, and the error is worth more than the number.
	//   IT DERIVED A NOISE FLOOR AND USED IT AS A DECISION THRESHOLD. Those are
	//   different objects:
	//
	//     DETECTION  -- is the asymmetry distinguishable from 1.0? This is
	//                   COUNT-DEPENDENT. At ~20 per arm the floor is ~30%; at
	//                   ~5,000 per arm it is ~2%, and 1.53x is then over 20
	//                   sigma from 1.0. A real asymmetry, beyond doubt.
	//     CONSEQUENCE -- is an asymmetry of that SIZE worth acting on? This is
	//                   NOT count-dependent. It is a judgement about what
	//                   magnitude of missing terrain matters, and more samples
	//                   never change it.
	//
	//   The gate needs the second. It was given the first.
	//
	//   THE BAR IS LEFT AT 1.5 ANYWAY, deliberately, and this note is written
	//   AFTER seeing a marginal 1.53x. Re-deriving a threshold once a result is
	//   in sight is the failure this whole workstream has been avoiding, and a
	//   tighter bar chosen now would be indistinguishable from one chosen to
	//   make 1.53 fail. So: the pre-registered bar says FAIL, and FAIL stands.
	//
	//   WHAT TO DO INSTEAD, for whoever sets the next one: derive the
	//   consequence threshold from something OUTSIDE this measurement -- what
	//   density of missing terrain is visible in a frame, and at what voxel
	//   size -- and register it before the leg that tests it. Report detection
	//   and consequence as two lines that can disagree, never as one verdict.
	//
	//   TWO GUARDS, both stated before the next leg runs:
	//     - if ill-posed >= 25% of classified rays, THE UNCONTESTED COUNTS ARE
	//       ALSO UNREADABLE, not just the contested verdict. An ill-posed ray
	//       is one whose only candidate failed validity, so it is drawn from
	//       the uncontested population too and suppresses these very counts.
	//     - if EITHER ARM is below 20, the counts are too small to divide;
	//       report the raw pair and decline a ratio. CORRECTED 2026-08-20: this
	//       was implemented as flat-only + hier-only < 20, i.e. the TOTAL, and
	//       so printed FAIL verdicts of 4.09x, 13.50x and 1.63x over
	//       denominators of 11, 2 and 19. A guard on a ratio must test the
	//       DENOMINATOR; testing the sum lets one large arm carry a denominator
	//       of 2 past it. Same defect class as doctrine 9e -- the guard was
	//       written correctly in one place and wrongly in another.
	//
	//   WHAT THIS GATE IS WORTH AT LEVEL 0, AND WHY IT MUST BE RE-RUN LATER.
	//   RETRACTED 2026-08-20: this read "~20 rays per leg in 1.35M is 1.5e-5 of
	//   the frame -- not worth a third brick-loop rewrite". THAT COUNT WAS AN
	//   ARTEFACT. Both the sentinel bug and the arbiter's own degenerate-span
	//   rejection discard uncontested rays before they are counted.
	//
	//   AND THE ASYMMETRY DID NOT SURVIVE EITHER. Once all THREE of the
	//   arbiter's strict boundary comparisons carried the derived tolerance,
	//   the one leg with statistical weight read flat-only 5,081 against
	//   hier-only 4,124 -- 1.23x, a PASS -- where the same leg had reported
	//   11.26x a round earlier. The filter was not merely suppressing the
	//   magnitude; it removed flat-side and hier-side candidates at DIFFERENT
	//   RATES and so manufactured the asymmetry. Every figure quoted before
	//   that point (2.2x, 5.5x, 7.33x, 11.26x, 58.6x) is an artefact.
	//
	//   DO NOT QUOTE A SUPPRESSED COUNT AS A DECISION BASIS -- not for its
	//   magnitude and not for its SHAPE. The directional skip defect is NOT
	//   ESTABLISHED; it may not exist. No readable verdict exists yet in
	//   either direction.
	//   BUT THE SEVERITY SCALES WITH LEVEL: a
	//   skipped brick is 0.8 m at level 0 and 25.6 m at level 5. The same
	//   defect that is invisible here would be a hole the size of a building in
	//   the cascade. THIS GATE MUST RUN AT EVERY LEVEL B-2b ADDS, and the bar
	//   must be set before that cascade is built, not after seeing what it
	//   gives.
	//
	// ===================================================================
	// v3, PRE-REGISTERED 2026-08-19 FOR THE BRICK-POOL SWAP -- WRITTEN
	// BEFORE THE SOURCE EXISTS, LET ALONE BEFORE A NUMBER FROM IT
	// ===================================================================
	//
	// THE HAZARD. Once the marcher reads the brick pool across ring levels, it
	// picks its LOD PER RAY from the cone rule while the quad mesher picks ONE
	// LOD PER CHUNK. Where those two choices differ, the two renderers are
	// drawing DIFFERENT GEOMETRY BY DESIGN. Those surfaces are smooth, so the
	// silhouette edge mask will not credit them, and they land in the INTERIOR
	// population -- which is the half the gate treats as having no excuse
	// available. That is a mechanism for a real interior FAIL with no defect
	// behind it, and it would arrive exactly when the pressure to explain a
	// number away is highest.
	//
	// THE RULE, FIXED NOW:
	//
	//   A compared pixel is RING-DISCORDANT if its hit distance t lies within
	//   +/- ONE CHUNK WIDTH of a ring transition radius, measured at the
	//   COARSER of the two levels meeting there (chunk width at level L is
	//   32 voxels = 3.2 m * 2^L).
	//
	//   Ring-discordant pixels are counted in their OWN column, are EXCLUDED
	//   from the interior gate, and are reported with their own rate and their
	//   own control. They are never folded into agreement.
	//
	//   The interior gate therefore applies to (interior AND NOT ring-discordant).
	//
	// WHY THAT PREDICATE AND NOT A BETTER ONE. The honest classifier would ask
	// whether the marcher's cone level differs from the ring level of the chunk
	// the RASTER surface belongs to -- but the raster path does not tell us its
	// per-pixel ring level, and inventing a channel for it is a bigger change
	// than this gate is worth. Hit distance against the known ring radii is
	// computable from the marcher's own t, needs nothing from the other side,
	// and is decided here rather than after seeing a failure.
	//
	// WHY ONE CHUNK AND NOT TWO. A ring transition is a chunk-granular event:
	// the mesher's LOD choice changes at a chunk boundary, so the widest a
	// genuine by-design difference can be is the chunk straddling the radius,
	// plus its neighbour on either side at worst. One chunk width is that,
	// stated in advance. WIDENING IT LATER TO MAKE A NUMBER PASS WOULD BE
	// VISIBLE IN THIS COMMENT'S HISTORY, WHICH IS THE POINT OF WRITING IT NOW.
	//
	// AND IT DOES NOT APPLY TO P3-B1. That step is level 0 only with no ring
	// transitions, so no pixel can be ring-discordant and the column must read
	// ZERO. A non-zero ring-discordant count on a level-0-only run is itself a
	// defect -- the classifier firing where no ring boundary is traversed --
	// which makes P3-B1 a free correctness test of this rule before the rule is
	// ever needed.
	//
	// ===================================================================
	// TWO THINGS THAT ARE NO LONGER TRUE AND MUST NOT BE QUOTED
	// ===================================================================
	//
	// 1. THE COST MODEL `0.13 + steps * 5.9 us` IS DEAD UNTIL RE-MEASURED. It
	//    was measured on a 1-bit flat volume where a solidity test is three
	//    integer ops and one load. The brick pool is FIVE indirections --
	//    chunk index, chunk record, brick descriptor, occupancy dwords, and
	//    material payload on hit -- of which two (the L1 mask bit test and the
	//    uniform-brick short circuit) are free and pull the cost DOWN while the
	//    rest push it up. The net is unknown. Every projection in
	//    docs/ray-marching-plan-2026-08-19.md rests on this model, so nothing
	//    downstream of it should be planned until P3-B1 reports.
	//
	// 2. THE 9.19x SKIP RATIO DOES NOT TRANSFER. It was measured against a
	//    two-level mip pyramid over the flat 512^3 volume -- a different
	//    acceleration structure with different cell sizes and different restart
	//    behaviour from the chunk/brick/cell hierarchy the pool implies. It is
	//    INDICATIVE, NOT PREDICTIVE. If the hierarchical walk gives materially
	//    less, that is a finding about the project and it gets surfaced as one;
	//    it is not a defect to be tuned back toward 9.19.
	struct FDepthGate
	{
		bool bValid = false;
		uint64 Generation = 0;

		uint32 Compared = 0;         // pixels where the marcher hit and raster had geometry
		uint32 Disagree = 0;         // |dz| > 0.5 * voxelSize(level)
		uint32 DisagreeEdge = 0;     // ... inside the dilated silhouette mask (excused)
		uint32 DisagreeInterior = 0; // ... outside it. THE NUMBER THAT MUST BE ~0.
		uint32 EdgePixels = 0;       // how much of `Compared` the exemption covers
		uint32 HitOnSky = 0;         // marcher drew where the raster drew nothing
		uint32 Miss = 0;             // marcher missed (mostly: past the volume's 25.6 m)
		uint32 MaxDeltaMilliVoxels = 0;
		uint32 Buckets[6] = {};      // |dz|/voxel: <0.25 <0.5 <1 <2 <4 >=4

		// INTERIOR ONLY, and these are the ones to read. A max over all compared
		// pixels is dominated by silhouettes, where the two renderers disagree
		// about WHICH SURFACE the pixel belongs to rather than about the depth of
		// one -- so the delta is the scene's depth range, not an error magnitude.
		// The first gate run reported 594 voxels that way and it looked like a
		// catastrophe; it was the statistic, not the marcher.
		uint32 InteriorMaxMilliVoxels = 0;
		uint32 InteriorBuckets[6] = {};

		// ---- the control, and it is the denominator the verdict needs -------
		//
		// The raster depth compared against a ONE-PIXEL-DISPLACED copy of
		// itself, on the same pixels, through the same threshold and the same
		// edge mask. Both sides are the reference, so this is what the
		// instrument scores when the marcher is not involved.
		//
		// It is an UPPER BOUND: one pixel is coarser than the sub-pixel
		// difference that actually separates the two renderers. So
		//   marcher above it -> a hard result
		//   marcher below it -> consistent with sampling noise, NOT proof
		// -- the same character, and the same wording, as the march spike's
		// restart-noise control.
		uint32 RefSelfDisagree = 0;
		uint32 RefSelfDisagreeInterior = 0;
		uint32 RefSelfMaxMilliVoxels = 0;

		// HOW MANY SAMPLED FRAMES THIS RESULT COVERS. Every field above is
		// summed over this many readbacks (the two max fields take a max).
		//
		// A single frame was not a comparable unit: the readback ring hands back
		// whichever frame had a slot free, so two consecutive reads of a moving
		// scene were two arbitrary frames and could not be told apart from a
		// drifting result. A fixed window can converge and two reads of a
		// settled system can be compared.
		uint32 SampleFrames = 0;
	};
	FDepthGate DepthGate;

	// Frames declined, BY REASON. Not one flag: "the cvar is on but nothing
	// drew" has four causes here and they need different fixes.
	uint64 DeclinedNoView = 0;      // PreRenderView never stashed a view
	uint64 DeclinedNoVolume = 0;    // no occupancy volume published yet
	uint64 DeclinedNoTextures = 0;  // scene textures unavailable at the hook
	uint64 DeclinedUnsupported = 0; // RHI/feature level refuses the pass
	uint64 DeclinedNonPrimary = 0;  // reflection/scene-capture view, not the player's
};

// ---------------------------------------------------------------------------
// Cross-thread state
// ---------------------------------------------------------------------------
class VOXELEARTHSHADERS_API FVoxelMarchState
{
public:
	static constexpr int32 kNumTimingPairs = 4;

	mutable FCriticalSection Lock;

	// -- written by the game thread under Lock --
	TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe> Volume;

	// -- written by the render thread under Lock --
	FVoxelMarchStats Stats;

	// -- render-thread-only --
	struct FTimingPair
	{
		FRenderQueryRHIRef Begin;
		FRenderQueryRHIRef End;
		bool bInFlight = false;
	};
	// SEPARATE RINGS, not a shared one. The two brackets live in the same frame;
	// a shared ring hands whichever pass grabs the free pair first, so the
	// reported number would silently alternate between two different
	// measurements. Four more query pairs is the whole cost of that being
	// impossible -- the same argument the fluid renderer's MarchTimingRing
	// makes against sharing with the solver's.
	FTimingPair MarchTiming[kNumTimingPairs];
	FTimingPair EmitTiming[kNumTimingPairs];
	FTimingPair ScratchTiming[kNumTimingPairs];
	// The fourth ring, exactly as the DepthPreEmit note in the .cpp said a
	// fourth bracket would mean: the bound producer (voxel.March.Bound) --
	// list pass + per-level cube rasters, opened before the first producer
	// pass and closed after the last, OUTSIDE the March bracket so marchMs
	// keeps meaning what every archived leg measured.
	FTimingPair BoundTiming[kNumTimingPairs];

	// THE HIT-TILE COUNT, READ BACK.
	//
	// It is the emit's DrawIndirect InstanceCount, and it is the number that says
	// whether the indirect emit is buying anything: hit == classified means the
	// tile reduction found nothing to skip and the whole screen is paying HTILE
	// decompression, which is the cost item (c) of the header budgets. Without
	// this it is a claim rather than a measurement.
	//
	// Three slots, same shape and same reason as the fluid renderer's census
	// ring: the GPU may be a frame or two behind, the render thread must not
	// stall, and a frame that finds no free slot is COUNTED rather than silently
	// dropped. Four bytes per slot.
	static constexpr int32 kNumTileReadbacks = 3;
	struct FTileReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		bool bInFlight = false;
	};
	FTileReadback TileRing[kNumTileReadbacks];
	uint64 TileReadbackSkips = 0;

	// THE HOLE METRIC'S READBACK RING (voxel.March.HoleStats). Its own ring,
	// not the tile ring, for the reason the verify ring is separate: the two
	// are enqueued under different cvars, and a shared ring would hand
	// whichever pass grabbed a slot first, silently alternating what the
	// number means. Three slots absorb the GPU running 1-3 frames behind
	// without a stall; a frame that finds no free slot goes unmeasured and
	// biases nothing, because the census divides by frames LANDED, not frames
	// dispatched (the shadow march's rule, kept). Latency to the perf line is
	// therefore 1-3 frames of readback plus up to one 5 s log period --
	// irrelevant against a 5 s window, and the window edges smear by at most
	// 3 frames of ~1.3M rays each.
	static constexpr int32 kNumHoleReadbacks = 3;
	struct FHoleReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		bool bInFlight = false;
		// Which permutation level the kernel that filled this slot ran at,
		// recorded AT ENQUEUE. The cvar can change in the 1-3 frames a
		// readback is in flight, and folding a level-1 frame's structurally
		// zero breakdown words into the window would print "0 evicted" for a
		// frame that measured no such thing -- the exact zeros-as-measurement
		// mistake this week's two retractions were made of.
		int32 ArmLevel = 0;
	};
	FHoleReadback HoleRing[kNumHoleReadbacks];

	// THE TIGHT RESIDENT-Z REDUCE'S READBACK RING (voxel.March.ZTight). Its
	// own ring, not the hole ring, for the standing reason: different cvar,
	// different buffer, different landing consumer -- and this one is not a
	// counter window at all, it is the ARM'S INPUT. Three slots absorb the
	// GPU running 1-3 frames behind; a frame with no free slot simply keeps
	// consuming the previous landed snapshot, which the staleness gate at the
	// fill site prices (a snapshot older than the age cap refuses every
	// slab -- lost benefit, never a hole).
	static constexpr int32 kNumZTightReadbacks = 3;
	// 7 ring levels -- the same count VoxelMarchHoleWord::kNumLevels names,
	// reused rather than respelled so the two move together.
	static constexpr int32 kZTightLevels = VoxelMarchHoleWord::kNumLevels;
	struct FZTightReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		bool bInFlight = false;
	};
	FZTightReadback ZTightRing[kNumZTightReadbacks];
	// One landed reduce, decoded. Count[L] == 0 means level L held no live
	// validated record in that frame's table and MUST be refused a slab --
	// the min/max fields are then meaningless sentinels, and bValid does not
	// vouch for them; only Count does, per level. Under Lock.
	struct FZTightSnapshot
	{
		int32 MinZ[kZTightLevels] = {};   // tight chunk-Z, unpadded
		int32 MaxZ[kZTightLevels] = {};   // inclusive, unpadded
		uint32 Count[kZTightLevels] = {}; // live validated records at L
		uint32 Phantoms = 0;              // non-zero records that failed validation
		uint32 Live = 0;
		uint32 Cover = 0;
		uint32 Examined = 0;              // must equal the dispatch's slot count
		uint64 LandFrame = 0;             // GFrameCounterRenderThread at landing
		bool bValid = false;
	};
	// The newest landed reduce -- ALSO the drift check's baseline: each
	// landing is compared against this before replacing it, so no second
	// copy exists to fall out of step with the one being served.
	FZTightSnapshot ZTightLanded;
	// THE SETTLE GATE. Non-zero refuses every slab; set to the settle cvar on
	// arming, on the first landing, and whenever a landing's bounds moved
	// outward past the pad since the previous landing (a discontinuity: cold
	// fill, teleport, a cliff crossing the ring edge). Decremented once per
	// fill while the arm is on. This is the CPU-side gate the pad argument
	// leans on for everything beyond one interval's ordinary drift.
	int32 ZTightSettleFramesLeft = 0;
	uint64 ZTightDiscontinuities = 0; // cumulative since boot, for the log
	uint64 ZTightLandings = 0;        // cumulative landings, for the log
	uint64 ZTightPhantomsCum = 0;     // cumulative phantom records, for the log

	// Accumulated under Lock as readbacks land; drained by
	// VoxelMarchGetAndResetHoleStats (bArmed is filled at drain, from the arm).
	FVoxelMarchHoleStats HoleWindow;
	// The last window the drain handed out, kept verbatim for the HUD's
	// peek (VoxelMarchPeekLastHoleWindow) -- the panel reads at 1 Hz, the log
	// drains at 5 s, and two drainers of one accumulator would each get a
	// random share. Under Lock.
	FVoxelMarchHoleStats LastDrainedHoleWindow;

	// The depth gate's own readback ring. Separate from the tile ring: the two
	// are enqueued by different passes under different cvars, and a shared ring
	// would hand whichever grabbed a slot first, so the reported gate would
	// silently alternate with a tile count.
	static constexpr int32 kNumVerifyReadbacks = 3;
	static constexpr int32 kVerifyWords = 24; // mirrors VOXEL_MARCH_VERIFY_WORDS
	struct FVerifyReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		uint64 Generation = 0;
		bool bInFlight = false;
	};
	FVerifyReadback VerifyRing[kNumVerifyReadbacks];
	uint64 VerifyGeneration = 0;

	// The join probe's readback. One slot: it is an on-demand diagnostic run
	// against a static pose, not a per-frame instrument, so a ring would only
	// add ways for it to report a different frame than the one asked about.
	// THE CPU IS THE AUTHORITY AND THE SHADER IS TOLD, rather than both being
	// written down and expected to match.
	//
	// This was `= 60; // mirrors VOXEL_MARCH_VIDX_WORDS` against a shader that
	// computed 64. MarchOutVerifyIndex is a typed UAV, so D3D12 DISCARDED the
	// four out-of-range writes -- no crash, no corruption, and sample 5 read back
	// as RecOrigin (x, 0, 0), slot 0, LevelAndFlags 0. NOT A BLANK: A PLAUSIBLE
	// WRONG VALUE, and one that reads as "the record claims level 0" when the
	// record said nothing at all.
	//
	// A constant carrying the comment "mirrors X" is a join that only a human
	// re-reading two files can verify, and this project has been bitten by that
	// shape five times in three days. These three numbers are pushed into the
	// shader by FVoxelMarchVerifyIndexCS::ModifyCompilationEnvironment, exactly
	// as VOXEL_MARCH_TILE_SIZE already is, and the total below is derived from
	// the same three. The two sides can no longer disagree because there is only
	// one side.
	static constexpr int32 kIndexProbeHeaderWords = 16;
	static constexpr int32 kIndexProbeSampleWords = 8;
	static constexpr int32 kIndexProbeSampleMax   = 6;
	static constexpr int32 kIndexProbeWords =
		kIndexProbeHeaderWords + kIndexProbeSampleMax * kIndexProbeSampleWords;
	TUniquePtr<FRHIGPUBufferReadback> IndexProbeReadback;
	bool bIndexProbeInFlight = false;

	static constexpr int32 kSourceCompareWords = 1772; // mirrors VOXEL_MARCH_VSRC_WORDS
	TUniquePtr<FRHIGPUBufferReadback> SourceCompareReadback;
	bool bSourceCompareInFlight = false;
	bool bSourceCompareOccValid = false;
	// The arm the CPU BUILT THE FRAME FOR, stashed at dispatch so it can be
	// compared against the arm the shader says it was COMPILED for. Two numbers
	// that must agree; the whole ring investigation turned on nobody having the
	// second one.
	bool bSourceCompareArmRingsCpu = false;
	// The view rect the compare dispatch covered. Needed to undo the inverted
	// min stored for the disagreement bounding box, and stashed at DISPATCH for
	// the same reason the arm is: the readback lands frames later and the rect
	// may have changed by then.
	FIntPoint SourceCompareViewSize = FIntPoint(1, 1);

	// HOW LONG THE INDEX HAS BEEN QUIET. The comparator refuses to sample a
	// volume that is still accepting chunks -- see the cvar. Tracked here rather
	// than in the index because "settled" is a property of the measurement, not
	// of the index.
	uint64 LastSeenIndexUploads = 0;
	int32 FramesSinceIndexChanged = 0;

	// The open accumulation window. Landed readbacks are summed here and only
	// published to Stats once the window is full, so what an operator reads is
	// always a completed sample of a stated size rather than whichever frame
	// happened to finish last.
	FVoxelMarchStats::FDepthGate VerifyAccum;
	int32 VerifyAccumFrames = 0;

	FVoxelMarchStats GetStats() const
	{
		FScopeLock Guard(&Lock);
		return Stats;
	}
};

// ---------------------------------------------------------------------------
// The publisher -- the single wire into this workstream
// ---------------------------------------------------------------------------
//
// Called from the game thread by whoever owns the volume. Creates the view
// extension on first call and refreshes the pointer on every subsequent one.
// Passing a null volume is legal and means "the source went away" (an
// Enable-cycle, a world teardown); the extension then declines every frame and
// counts it as DeclinedNoVolume rather than crashing or drawing stale terrain.
//
// Safe to call every tick. Costs one lock when nothing has changed.
// CREATE THE MARCH EXTENSION, INDEPENDENTLY OF THE FLUID SUBSYSTEM.
//
// The extension used to be created only inside VoxelMarchPublishSource, which
// the FLUID subsystem calls -- so with voxel.Fluid.Enable 0 the marcher did not
// merely lack an occupancy volume, IT DID NOT EXIST, and terrain rendering was
// gated on the water simulation running. Idempotent, game thread; call it from
// whoever owns the brick pool.
VOXELEARTHSHADERS_API void VoxelMarchEnsureExtension(UWorld* World);

// THE OTHER HALF OF EnsureExtension, and it is not optional.
//
// GMarchExtension is a process-lifetime global created ONCE, guarded by
// if (!GMarchExtension.IsValid()), and associated with the UWorld it was made
// for. Nothing released it, so the guard stayed satisfied forever: PIE session
// two called EnsureExtension, found a valid pointer bound to session ONE's
// dead world, and returned without registering anything. Terrain streamed
// normally and was never drawn, while water -- which has its own quad pools
// and no dependency on this extension -- kept rendering. The owner saw lakes
// floating in an empty sky, three sessions running.
//
// UVoxelShadowMarchSubsystem never had this bug because it is a WORLD
// subsystem: its Extension and State are members, rebuilt in OnWorldBeginPlay
// and dropped in Deinitialize. This is that same teardown for the global pair.
// Call it from the world's Deinitialize, next to the pool and index teardown --
// same class of bug, same fix, third instance today.
VOXELEARTHSHADERS_API void VoxelMarchReleaseExtension();

VOXELEARTHSHADERS_API void VoxelMarchPublishSource(
	UWorld* World,
	const TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe>& InVolume);

// The live stats, for whoever prints the perf line. Returns a zeroed struct
// with Frames == 0 when the extension has never been created -- which is the
// "nobody asked for anything" answer and is distinct from "it ran and did
// nothing".
VOXELEARTHSHADERS_API FVoxelMarchStats VoxelMarchGetStats();

// ---------------------------------------------------------------------------
// THE STREAMING SYSTEM'S OWN CONVERGENCE SIGNAL
// ---------------------------------------------------------------------------
//
// Called once per tick from whoever owns the streaming counters. GAME THREAD.
//
// WHY THE COMPARATOR NEEDS THIS AND CANNOT SYNTHESISE IT. Its first settle gate
// waited for the chunk index to go quiet; a 60-frame upload-quiet window during
// streaming is a LULL, not convergence, and this project's cold-fill tooling
// already records a leg that read one as settled and made the GPU fork look 12%
// faster than it was.
//
// Its second gate compared index entries against the pool's level-0 census --
// better, but measured against a MOVING TARGET: the pool itself settles at
// 97,019 / 97,082 / 97,471 across otherwise identical runs, so the index can
// faithfully match a pool that holds a slightly different world each time.
//
// The signal that does not move is the streaming system's own: jobsInFlight 0,
// pendingJobs 0, chunk count at its peak and held. Every other gate on this
// project already uses it -- the brick byte-equality gate reads only a
// converged pool for exactly this reason.
//
// UNWIRED IS LOUD, NOT SILENT. If this is never called, the comparator says so
// on every refusal rather than quietly falling back to the weaker condition. A
// gate that degrades without announcing it is the failure class this whole
// workstream has been fighting.
VOXELEARTHSHADERS_API void VoxelMarchPublishStreamingState(int32 JobsInFlight, int32 PendingJobs,
                                                           int64 ChunksLoaded);

// The convergence counter the line above maintains, readable from any thread.
//
// EXPOSED RATHER THAN RE-DERIVED. The render-frame split needs to know where
// the cold-fill boundary is so that a fill frame is not folded into the settled
// population the owner's gate is about, and the streaming subsystem already
// publishes exactly that signal every tick. A second settle rule computed from
// a different signal is how two instruments come to disagree about which frames
// they described while both look healthy.
//
// -1 means the publisher has never been called: the wire is DEAD, not settled.
// A reader must treat that as "unknown", never as "not yet converged".
VOXELEARTHSHADERS_API int32 VoxelMarchGetStreamConvergedFrames();

// THE VERDICT, not the raw count -- so the settle threshold lives in exactly one
// place. voxel.March.SettleFrames is applied here, inside the file that owns it,
// rather than being duplicated by every reader; a constant copied into a second
// file is how two instruments come to disagree about the same boundary while
// both look right. Returns false when the publisher has never been called: an
// unwired signal is NOT a settled world.
VOXELEARTHSHADERS_API bool VoxelMarchIsStreamConverged();

// ---------------------------------------------------------------------------
// The extension
// ---------------------------------------------------------------------------
class VOXELEARTHSHADERS_API FVoxelMarchRenderExtension : public FWorldSceneViewExtension
{
public:
	FVoxelMarchRenderExtension(const FAutoRegister& AutoRegister, UWorld* InWorld,
	                           TSharedPtr<FVoxelMarchState, ESPMode::ThreadSafe> InState);

	//~ Begin ISceneViewExtension
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	// ANCHOR A AND ANCHOR B OF THE RENDER-FRAME SPLIT, and they exist for no
	// other reason. See VoxelRenderFrame.h. The marcher does no rendering work
	// in either: PreRenderViewFamily is the FIRST render-thread hook the engine
	// calls (SceneRendering.cpp:4299) and PostRenderViewFamily is the LAST
	// (SceneRendering.cpp:4956), so together they bracket the scene renderer's
	// graph-construction half exactly, and everything outside them is the
	// render thread's OTHER work -- which is what the split is trying to see.
	// With -VoxelRenderFrame=0 both fold to one branch on a latched int.
	virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder,
	                                              FSceneViewFamily& InViewFamily) override;
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder,
	                                               FSceneViewFamily& InViewFamily) override;

	// Stash. The ONLY hook that is handed a view before the base pass, which is
	// why the march -- which needs a camera and a projection -- cannot live
	// where it runs without it.
	virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder,
	                                        FSceneView& InView) override;

	// Classify + march. No view argument by design (the engine calls this once
	// per family, not once per view), hence the stash.
	virtual void PreRenderBasePass_RenderThread(FRDGBuilder& GraphBuilder,
	                                            bool bDepthBufferIsPopulated) override;

	// Emit. Handed the base pass's own render-target bindings and the scene
	// texture uniform buffer -- see 1 above.
	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder, FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;
	//~ End ISceneViewExtension

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	// Everything the march needs about one view, captured at
	// PreRenderView_RenderThread and consumed at the two later hooks.
	//
	// KEYED BY THE FSceneView POINTER, and that is the only key available: the
	// classify hook gets no view at all, and the emit hook gets a view whose
	// identity must be matched back to the march output that belongs to it.
	// Stale entries are dropped by frame number rather than by unregistering,
	// because a view that is culled between the two hooks never tells anyone.
	struct FViewMarch
	{
		const FSceneView* ViewKey = nullptr;
		uint32 FrameNumber = 0;

		FIntRect ViewRect;
		FMatrix44f ViewToTranslatedWorld;
		FVector3f RayOriginLocalUU = FVector3f::ZeroVector;
		FVector2f InvProjDiag = FVector2f(1.0f, 1.0f);

		// THE FRAME'S TAA/TSR JITTER, IN NDC, OR ZERO (voxel.March.TAAJitter).
		//
		// Resolved ONCE here, on the render thread, from the same FViewMatrices
		// the diagonal above came from -- so the march, the emit's ray
		// reconstruction and the verify gate cannot read three different phases
		// of one frame. Zero when the arm is off, which is the shipping arm and
		// reproduces every capture in the archive exactly.
		//
		// STASHED RATHER THAN READ FROM ResolvedView IN THE SHADER, and that is
		// not a style choice. VoxelMarchBuildRay runs in the MARCH COMPUTE
		// KERNEL, which never calls ResolveView(); InstancedStereo.ush:52
		// declares ResolvedView as a ZERO-INITIALISED static, so
		// ResolvedView.TemporalAAJitter there is silently (0,0) and the arm
		// would read as armed and do nothing. This renderer has already lost
		// days to exactly that -- see the ResolvedView note at VoxelMarch.usf.
		FVector2f TemporalAAJitter = FVector2f::ZeroVector;

		FVector4f InvDeviceZToWorldZ = FVector4f(0, 0, 0, 0);

		// THE CONE SLOPE. World-space pixel half-width per unit of distance
		// along the ray -- the quantity the whole LOD rule is written in, and
		// the reason there is not a single ddx/ddy in this renderer. Computed
		// once here from the projection and the viewport; see the .cpp for the
		// derivation, which is two lines and worth reading before changing it.
		float PixelConeSlope = 0.0f;

		// The camera, in LEVEL-0 WORLD VOXELS, differenced in double and floored.
		//
		// THIS IS WHAT MAKES A MEASUREMENT REPEATABLE. The march frame used to be
		// anchored on FVoxelFluidOccupancyVolume's origin, which is
		// DesiredOriginVoxel(camera) run through RecentreTo's hysteresis -- so it
		// depends on the HISTORY of camera motion and on whether the volume
		// happened to recentre this run. Two byte-identical legs therefore got
		// different clip boxes, different segments, and different rays.
		//
		// Measured: two identical runs disagreed by 102x on the comparator's
		// total and 10x on its hit count. That was not noise around a value, it
		// was a different population being judged, and it made seven rounds of
		// per-term deltas unreadable.
		//
		// Anchored on the camera and snapped to a chunk, the frame is a pure
		// function of the pose. Same pose, same frame, same rays.
		FIntVector CameraVoxel = FIntVector::ZeroValue;
		// The camera in world UU, kept in DOUBLE. The frame origin is differenced
		// against this rather than shifted from the old frame, because the old
		// frame is exactly the quantity being removed from the measurement.
		FVector ViewOriginUU = FVector::ZeroVector;

		// ---- the LOCAL FRAME the march actually used ----------------------
		//
		// STASHED, NOT RECOMPUTED, and that is a correctness requirement rather
		// than a convenience. voxel.March.ReachM re-anchors the frame on the
		// camera; the march resolves it at PreRenderBasePass and the EMIT
		// reconstructs the ray from it at PostRenderBasePassDeferred to compute
		// SV_Depth. If the two derived it independently -- one from the reach,
		// one from the occupancy volume -- the emit would project a hit the
		// march never found, and the symptom is terrain at the wrong depth
		// rather than an error.
		//
		// Defaults are the occupancy volume's own frame, which is what reach 0
		// resolves to.
		FIntVector FrameOriginVoxel = FIntVector::ZeroValue;
		FVector3f FrameRayOriginLocalUU = FVector3f::ZeroVector;
		float FrameExtentUU = 0.0f;
		// The frame's Z extent is its own budget (the altitude fix, 2026-09-02):
		// XY reach belongs to the ring cascade, Z reach to
		// kVoxelMarchAltitudeReachVoxels. Equal to FrameExtentUU whenever the
		// rings are off, so every non-ring path keeps its cube.
		float FrameExtentZUU = 0.0f;

		// The view uniform buffer, carried so the emit's pixel shader can reach
		// PrevPreViewTranslation / PrevTranslatedWorldToClip for velocity and
		// the DBuffer's screen-space setup. Held as the RHI ref the view owns;
		// it outlives both hooks within the frame.
		FRHIUniformBuffer* ViewUniformBuffer = nullptr;

		// March outputs, produced at PreRenderBasePass, consumed at the emit.
		FRDGTextureRef VisBuffer = nullptr;
		FRDGTextureRef HitDistance = nullptr;
		FRDGBufferRef EmitTileList = nullptr;
		FRDGBufferRef EmitDrawArgs = nullptr;
		FIntPoint TileCount = FIntPoint::ZeroValue;
		bool bMarched = false;
	};

	FViewMarch* FindView(const FSceneView* View);
	void RetireTimingQueries();

	TArray<FViewMarch> Views;
	TSharedPtr<FVoxelMarchState, ESPMode::ThreadSafe> State;
};
