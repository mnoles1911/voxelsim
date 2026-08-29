#pragma once
// VoxelEofDirtyLedger.h -- WHOSE COMPONENTS IS EndOfFrameUpdates PROCESSING?
//
// ===========================================================================
// WHAT THIS SETTLES
// ===========================================================================
// docs/game-thread-attribution-2026-08-28.md, "The largest named non-voxel
// term". On one moving leg, 9,000 rows, the engine's own game-thread scopes
// read:
//
//     Exclusive/GameThread/       FAST     TAIL     delta
//     EndOfFrameUpdates          0.141    3.785    +3.643
//     Tickables                  1.178    2.312    +1.134   (= the voxel tick)
//     everything else            <=0.10   <=0.14   <=+0.04
//
// `EndOfFrameUpdates` is the finding: +3.64 ms at the frame-time tail, AN
// ORDER OF MAGNITUDE above every other named non-voxel term. It processes
// components whose render state was marked dirty (and components registered or
// unregistered) since the last frame, so its cost is proportional to HOW MANY
// components were marked and by WHOM.
//
// THE OBVIOUS EXPLANATION IS ALREADY DEAD. Chunk publication marks render state
// dirty, so the natural reading is that this is streaming cost landing outside
// the voxel tick. It is not:
//
//     corr(EndOfFrameUpdates, ChunksApplied) = 0.047    <- essentially zero
//     mean when chunks == 0:  0.201 ms  (n=7,106)
//     mean when chunks  > 0:  0.308 ms  (n=1,894)
//
// A 0.107 ms difference cannot produce a 3.79 ms tail. And the shape is BURSTY
// AND CLUSTERED, not periodic: 375 of 9,000 frames over 2 ms, in runs of
// consecutive frames. That is something SPAWNING OR REGISTERING COMPONENTS
// across several frames, not a scheduled batch.
//
// The doc's pre-scoped identifying step was "a counter on each
// MarkRenderStateDirty call site, attributed by subsystem, read on a leg
// alongside this column". This file is that counter.
//
// ===========================================================================
// THE READING
// ===========================================================================
// One line per 5 s window, printed as a sibling of the other `(window)` lines
// from FVoxelWorldImpl::MaybeLogCounters, so it divides the same window the
// rest of the streaming instrument does. Read it AGAINST the CSV profiler's
// `Exclusive/GameThread/EndOfFrameUpdates` column over the same seconds.
//
//   THE SOURCE WHOSE COUNT CLUSTERS WHERE THE EOF COLUMN SPIKES IS THE OWNER.
//   Both quantities are bursty; the reading is co-location of the bursts, not
//   a ratio. A source that is large but FLAT across a window in which the EOF
//   column doubled is not the owner -- it is background traffic.
//
//   ALL-ZERO LEDGER WHILE THE EOF COLUMN SPIKES = THE MARKS COME FROM OUTSIDE
//   THIS MODULE, and the next step is engine-side (or the two out-of-scope
//   sources named below), not more counters in VoxelEarth.
//
//   chunkPublish= VALIDATES THE INSTRUMENT AND NOTHING ELSE. The correlation
//   above already ruled that source out, so on any moving leg it must read
//   NON-ZERO (the pipeline is publishing) and UNCORRELATED with the EOF spikes.
//   chunkPublish=0 on a moving leg means the ledger is not wired into the path
//   it claims to measure and NO verdict may be read from that leg -- that is
//   the failure this project has shipped eleven times in one night
//   (docs/... "silent success is the house failure"), and it is why the
//   already-excluded source is counted rather than omitted as redundant.
//
//   lakeAdopt= IS A CONTROL TERM AND DIRTIES NOTHING. Adoption reuses the
//   previous gather's component untouched -- no register, no mesh section, no
//   mark. It is in the ledger because the lake-sheet re-gather (per-basin
//   components, PendingDestroy drained 32/tick, adoption on gathers,
//   2026-08-28) is the leading HYPOTHESIS for a burst spread over several
//   frames, and the hypothesis has to be separable from its own null: if the
//   EOF spikes ride on lakeAdopt while lakeCreate/lakeDestroy stay flat, then
//   ADOPTION IS NOT THE MECHANISM and the correlation is coincidence of
//   timing. lakeCreate/lakeDestroy are the terms that can actually cost.
//
// ===========================================================================
// THERE IS NO `other=`. THE LEDGER IS AN ENUMERATED LIST, AND THAT IS A READING
// ===========================================================================
// A ledger with an `other=` bucket is strictly better, because a source you
// forgot shows up as a visible gap instead of as silence. THERE IS NO GLOBAL
// HOOK TO BUILD ONE FROM: the engine offers no public per-frame count of the
// components it queued for EndOfFrameUpdates, so nothing here can subtract the
// enumerated sources from a true total. Rather than fake one, this is stated:
//
//   THE LINE IS AN ENUMERATED LIST OF EVERY CALL SITE FOUND IN
//   ue-project/Source/VoxelEarth AS OF 2026-08-29. A SPIKING EOF COLUMN OVER A
//   FLAT LEDGER MEANS AN UN-ENUMERATED SOURCE -- it is not "no cause found".
//
// The un-enumerated sources that are known TODAY, so that reading has somewhere
// to go first:
//
//   1. Source/VoxelEarthShaders (a DIFFERENT MODULE, out of this instrument's
//      scope). UVoxelGpuPoolComponent marks render state dirty at
//      VoxelGpuPoolComponent.cpp:3180/3690/3701/4127/4676 and marks render
//      TRANSFORM dirty at :3883 on every PushUpdatesToProxy with a live proxy;
//      UVoxelGpuChunkComponent at VoxelGpuChunkComponent.cpp:228/235/248.
//      The pool's own per-window traffic is ALREADY REPORTED, on the
//      `Voxel pool publish (window)` line as `pushes=`; read that beside this
//      ledger before concluding the marks are engine-side.
//   2. Anything outside both modules -- engine actors, the pawn, Niagara, the
//      HUD, editor-only components.
//
// ===========================================================================
// COST, AND WHY PLAIN int64
// ===========================================================================
// Increments only. No timing brackets, no scopes, no allocation. Every counted
// call is an engine component API -- MarkRenderStateDirty, RegisterComponent,
// DestroyComponent, CreateMeshSection, AddInstances -- and every one of those
// is GAME-THREAD-ONLY by engine contract; a worker calling any of them is
// already a bug the engine asserts on. The ledger therefore inherits
// game-thread-only exclusivity from the very calls it counts, which is what
// makes plain int64 correct here rather than merely cheap. Nothing in this file
// may be called from a worker.
//
// House rule, applied: COUNT AT THE ++ SITE. Every increment sits on the same
// statement as the dirtying call, not in a parallel bookkeeping spot that can
// drift out of the path while still reading green.

#include "CoreMinimal.h"

namespace VoxelEofLedger
{
// One entry per SOURCE, where a source is "an owner in this module that
// performed an action this window which dirties render state or registers /
// unregisters a component". Sites provably on the per-chunk-publication path
// share ChunkPublish; see the reading note above for why an already-excluded
// source is still counted.
enum class ESource : uint8
{
	// UVoxelAgentSubsystem: AgentISM->MarkRenderStateDirty(). One count = one
	// whole-ISM scene-proxy destroy+rebuild (spawn batches and the per-tick
	// batched instance update, which deliberately marks ONCE for the batch).
	AgentISM = 0,
	// UVoxelDetailAssetSubsystem: per-HISM instance mutation (the append after a
	// group resolves, and the ClearInstances+AddInstances full rebuild) plus the
	// HISM's own RegisterComponent. One count = one component dirtied.
	Detail,
	// AVoxelWaterSheetActor: a basin's component was created and/or had its mesh
	// section (re)built or cleared -- a UProceduralMeshComponent proxy recreate.
	LakeCreate,
	// AVoxelWaterSheetActor: a basin ADOPTED intact across a re-gather. CONTROL
	// TERM -- this performs NO dirtying at all. See the reading note.
	LakeAdopt,
	// AVoxelWaterSheetActor: a parked leftover component actually destroyed by
	// the amortised PendingDestroy drain (32/tick).
	LakeDestroy,
	// AVoxelClipmapActor: clipmap level and underground-veil mesh sections, and
	// the veil / cave post-process / cave lamp component registrations.
	Clipmap,
	// AVoxelRiverRibbonActor: per-path Create/ClearMeshSection on the shared PMC.
	Ribbon,
	// The terrain streaming path: UVoxelChunkComponent::SetChunkQuads, plus
	// chunk component acquire/return and the streamer's own root/pool
	// registrations. THE DOC ALREADY RULED THIS OUT; it is counted so the
	// instrument can be proven to be in the path at all.
	ChunkPublish,
	// UVoxelChunkComponent material churn: SetChunkMaterial's base-material swap
	// and ApplyRingFadeParams' first MID assignment. Separated from ChunkPublish
	// because it is a DIFFERENT trigger (ring fade, debug tint) that can fire
	// without a publication.
	ChunkMaterial,
	// UWaterChunkComponent::SetChunkQuads / SetMaterial -- near-field CA water
	// and implicit cavern water re-meshing. Many components per tick.
	WaterNear,
	// Near-field water component lifetime: UWaterChunkComponent NewObject +
	// RegisterComponent, and the DestroyComponent sites that drop a drained,
	// empty, mobilized or evicted brick.
	WaterComp,
	// UVoxelGISubsystem: the legacy/fallback Comp->MarkRenderStateDirty() when
	// UpdateGIVertexColors cannot take the cheap path, and the local-light
	// (torch) intensity marks.
	GI,
	// UVoxelSkySubsystem: sun cascade/shadow-distance marks at setup and the
	// height-fog sky-capture flag mark.
	Sky,
	// AVoxelDebris: one count per debris body's instance build (one ISM fill,
	// not one per instance).
	Debris,

	Count
};

// THE ++ SITE. N defaults to 1; pass a batch size only where one call really
// dirties N components.
void Count(ESource Source, int64 N = 1);

// Orthogonal roll-up across ALL sources: every RegisterComponent /
// DestroyComponent call site in this module also lands here, so the line can
// separate "many marks on few components" from "many components arriving".
// DELIBERATELY OVERLAPS the per-source columns -- it is a second view of the
// same events, not a partition of them, and the print says so.
void CountRegister(int64 N = 1);
void CountUnregister(int64 N = 1);

// Formats the one-line report and ZEROES the window. Called from
// FVoxelWorldImpl::MaybeLogCounters so this window is the same window every
// other `(window)` line divides by.
FString FormatAndResetWindow();

} // namespace VoxelEofLedger
