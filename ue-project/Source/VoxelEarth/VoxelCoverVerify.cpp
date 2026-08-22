// voxel.Cover.VerifyStore -- THE BYTE GATE ON THE COVER STORE.
//
// Phase 6 of docs/ray-marching-plan-2026-08-19.md; design and measurement in
// docs/detail-assets-in-the-volume-2026-08-19.md. Shaped after
// voxel.GPU.VerifyPoolWrite and voxel.GPU.VerifyBrickPack: a PRIVATE pool, a
// direct arm, control arms, and a guard band over every unallocated dword.
//
// ===========================================================================
// WHAT THIS PROVES THAT THE PRODUCER'S UNIT TESTS DO NOT
// ===========================================================================
//
// voxelcore/covervolume.h has five passing tests, so the PACK is right. They say
// nothing about the STORE: the suballocation, the descriptor rebase, the
// Lock/Memcpy/Unlock, the 32 B record and the level-7 key are all engine-side
// code that can be wrong while producing a pool that looks populated. This gate
// reads the arenas back and compares them to vxc::packCoverChunk over the SAME
// resolved instance list.
//
// THE SAME LIST, NOT THE SAME RECT, and CoverChunkResult::resolved exists for
// exactly this: a gate that re-derives the instance list from its own rect is
// checking the resolver and the store at once and cannot say which failed.
//
// ===========================================================================
// THE TOLERANCE IS EXACTLY ZERO, AND NOBODY MAY ADD AN EPSILON
// ===========================================================================
//
// Section 7.1: both sides are integer voxels and there is no float anywhere in
// this path. The standing rule "calibrate a verifier against the reference's own
// noise floor, not zero" is about the DEPTH gate and does not transfer here.
//
// ===========================================================================
// FOUR ARMS, AND THREE OF THEM EXIST TO FAIL
// ===========================================================================
//
// A gate that only ever compares correct bytes to correct bytes is proved
// against the easy case. Section 7.3: the only way to know a check can fail is
// to break the thing it watches and watch it go red -- and to break it in more
// than one place, so selectivity is shown rather than assumed.
//
// This project has now found instruments that reported AGREEMENT with the defect
// they existed to find: the VerifyIndex level literal, which matched the bug it
// was watching for, and a truncated probe sample that read back as
// "LevelAndFlags 0" -- i.e. "the record claims level 0" when the record said
// nothing at all. Both produced PLAUSIBLE WRONG VALUES rather than blanks. No
// amount of care catches that; only a deliberately-wrong input does.
//
// So every run performs all four:
//
//   DIRECT      the stored bytes against the reference, rebased by the
//               allocation the pool ACTUALLY handed out. Must agree exactly.
//   REBASE      the same stored bytes against the reference NOT rebased. Must
//               DISAGREE -- otherwise the rebase is a no-op and the direct arm
//               is comparing chunk-relative offsets to themselves. That is
//               format section 6b's trap, wired in as an arm instead of a note.
//   DISPLACED   chunk i's stored bytes against chunk (i+1)'s reference. Must
//               DISAGREE for every pair. If the comparator cannot tell two
//               different chunks apart it is not reading the bytes -- and
//               "everything is air" would pass a direct-only gate silently,
//               which is section 7.2's warning about a check that cannot fail.
//   BITFLIP     one bit flipped in a copy of the reference. Must be detected.
//               Proves single-bit sensitivity ON THIS RUN rather than on a run
//               somebody remembers to do.
//
// A failure of REBASE, DISPLACED or BITFLIP is reported as INSTRUMENT BROKEN and
// never as a cover defect. They are statements about the gate, not the store.
//
// ===========================================================================
// AND IT ABSTAINS RATHER THAN GUESSING
// ===========================================================================
//
// Gate v4 learned this expensively: a one-sided bar passed a 33x collapse, and a
// confident ratio was printed from 30-vs-13 samples. Below kMinChunks resident
// or kMinSolidVoxels solid cover cells this gate returns NO VERDICT and names
// the floor it missed. "The producer ran and found no cover here" is a real
// answer about the ground. It is not a pass.

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "VoxelBrickCpuPackFromCore.h"
#include "VoxelBrickPool.h"
#include "VoxelCoords.h"
#include "VoxelEarth.h"
#include "VoxelWorldSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

#include "voxelcore/amplifier.h"
#include "voxelcore/assetbank.h"
#include "voxelcore/assetchannels.h"
#include "voxelcore/assetfield.h"
#include "voxelcore/brickpack.h"
#include "voxelcore/core.h"
#include "voxelcore/covervolume.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelCoverVerify, Log, All);

namespace
{
	// The pitch the land detail library is baked at. Restated from
	// VoxelDetailAssetSubsystem.cpp's kCoverPitchMm; vxc::coverVolumeInit refuses
	// a pitch that does not tile the world lattice, so a disagreement between the
	// two is a refusal rather than a volume half a voxel low everywhere.
	constexpr uint32 kCoverPitchMm = 50;

	// ---- the abstain floors ------------------------------------------------
	//
	// Separate because they fail for different reasons: too few chunks means the
	// search found no cover-bearing ground; too few solid cells means it found
	// cover so thin that a byte comparison proves almost nothing. Either one
	// returns NO VERDICT, and the message says which.
	constexpr int32 kMinChunks = 4;
	constexpr int32 kMinSolidVoxels = 256;

	// How many cover chunks to admit, and how far to search for them. SMALL ON
	// PURPOSE: the whole arena is read back, and a private pool sized to the run
	// makes the guard band cover every byte the run could have touched rather
	// than a prefix of a 300 MiB buffer.
	constexpr int32 kDefaultChunks = 8;
	constexpr int32 kSearchRadiusGroups = 12;

	// One 2x2 block of level-0 chunks, as VoxelDetailAssetSubsystem resolves.
	constexpr int64 kGroupEdgeVoxels = 64;

	struct FCoverChunkUnderTest
	{
		FIntVector Coord = FIntVector::ZeroValue;   // COVER chunk coordinates
		vxc::ChunkBrickPack Cpu;                    // the reference
		FVoxelBrickPool::FResidentChunk Res;
		bool bResident = false;
		int32 SolidVoxels = 0;
	};

	// ---- the comparator, in ONE place --------------------------------------
	//
	// EVERY ARM CALLS THIS FUNCTION, and that is what makes the control arms
	// mean anything: a displaced comparison that ran through different code would
	// prove nothing about the direct one. Returns mismatching dwords; zero means
	// byte-identical.
	//
	// OccBase/MatBase are folded into the reference's descriptors before
	// comparing. Passing 0 for both IS the REBASE control arm.
	int32 CompareChunkBytes(const vxc::ChunkBrickPack& Ref, uint32 OccBase, uint32 MatBase,
	                        const TArray<uint32>& PoolDesc, uint32 BrickBase,
	                        const TArray<uint32>& PoolOcc, uint32 PoolOccBase,
	                        const TArray<uint32>& PoolMat, uint32 PoolMatBase)
	{
		int32 Fail = 0;

		// (1) the 64 descriptor slots, with the arena bases folded in.
		//
		// ONLY MIXED BRICKS CARRY OFFSETS. A uniform SOLID brick's MatWord holds
		// a MATERIAL in its low bits, not a dword offset, so rebasing it would
		// corrupt the material -- and the corruption would be invisible in any
		// per-brick test that only looked at mixed bricks. The 28-bit mask is
		// applied here because the writer masks; it is not assumed away.
		// THE CONSTANTS COME FROM voxelcore/brickpack.h, NOT FROM LITERALS HERE.
		// This arithmetic must reproduce FVoxelBrickPool's flush exactly; naming
		// the fields through the shared authority means a change to the
		// descriptor layout breaks this gate at COMPILE time instead of making it
		// quietly agree with a store it no longer describes. kBrickFieldMask has
		// no voxel-core spelling, so it is derived from the offset mask rather
		// than written out a third time.
		constexpr uint32 kFieldMask = ~vxc::kBrickOffsetMask;
		constexpr uint32 kKindMixed = 2u;
		for (int32 B = 0; B < int32(vxc::kMarchChunkBricks); ++B)
		{
			const vxc::BrickDesc& D = Ref.descs[size_t(B)];
			uint32 WantOcc = D.OccWord;
			uint32 WantMat = D.MatWord;
			// MIXED ONLY, and the pool's flush says why: a uniform brick's offset
			// fields are zero by contract and must stay zero, because rebasing
			// them points a collapsed brick at a real arena range -- which reads
			// as valid and is therefore worse than useless.
			if (D.kind() == kKindMixed)
			{
				WantOcc = (D.OccWord & kFieldMask)
				        | (((D.OccWord & vxc::kBrickOffsetMask) + OccBase)
				           & vxc::kBrickOffsetMask);
				WantMat = (D.MatWord & kFieldMask)
				        | (((D.MatWord & vxc::kBrickOffsetMask) + MatBase)
				           & vxc::kBrickOffsetMask);
			}
			const int32 Slot = int32(BrickBase) + B;
			if (!PoolDesc.IsValidIndex(Slot * 2 + 1))
			{
				++Fail;
				continue;
			}
			Fail += (PoolDesc[Slot * 2 + 0] != WantOcc) ? 1 : 0;
			Fail += (PoolDesc[Slot * 2 + 1] != WantMat) ? 1 : 0;
		}

		// (2) the occupancy dwords, byte for byte at the chunk's arena offset.
		for (int32 W = 0; W < int32(Ref.occ.size()); ++W)
		{
			const int32 I = int32(PoolOccBase) + W;
			if (!PoolOcc.IsValidIndex(I) || PoolOcc[I] != Ref.occ[size_t(W)])
			{
				++Fail;
			}
		}

		// (3) the material dwords -- local palette AND payload.
		for (int32 W = 0; W < int32(Ref.mat.size()); ++W)
		{
			const int32 I = int32(PoolMatBase) + W;
			if (!PoolMat.IsValidIndex(I) || PoolMat[I] != Ref.mat[size_t(W)])
			{
				++Fail;
			}
		}
		return Fail;
	}

	// The 32 B record, built through the pool's OWN writer rather than a
	// transcription of the format document.
	//
	// allSolid is the field that makes this worth a separate arm: it is NOT
	// derivable from the descriptors and looks as though it is, so an
	// implementation that derived it would pass every other check here.
	int32 CompareChunkRecord(const FCoverChunkUnderTest& C, const TArray<uint32>& PoolTable)
	{
		uint32 Want[FVoxelBrickPool::kChunkRecordDwords] = {};
		const int32 E = int32(vxc::kCoverChunkEdgeCells);
		FVoxelBrickPool::BuildChunkRecord(
			FIntVector(C.Coord.X * E, C.Coord.Y * E, C.Coord.Z * E),
			uint32(FVoxelBrickPool::kCoverLevel), C.Cpu.anySolid, C.Cpu.allSolid,
			C.Res.BrickBase, C.Cpu.brickSolid,
			// NEUTRAL, and it must be: cover is 50 mm vegetation at the cover
			// level. It has no biome tint and no surface gate, and the producer
			// (VoxelDetailAssetSubsystem) passes neutral too -- so the reference
			// and the producer agree by construction rather than by luck.
			FVoxelBrickChunkShading::Neutral(), Want);

		int32 Fail = 0;
		const int32 Base = int32(C.Res.ChunkSlot) * FVoxelBrickPool::kChunkRecordDwords;
		for (int32 W = 0; W < FVoxelBrickPool::kChunkRecordDwords; ++W)
		{
			if (!PoolTable.IsValidIndex(Base + W) || PoolTable[Base + W] != Want[W])
			{
				++Fail;
			}
		}
		return Fail;
	}

	// ===================================================================
	// The run
	// ===================================================================
	struct FCoverVerifyRun : public TSharedFromThis<FCoverVerifyRun, ESPMode::ThreadSafe>
	{
		FVoxelBrickPool Pool;   // PRIVATE to this run -- see the header
		TArray<FCoverChunkUnderTest> Chunks;
		int32 WantChunks = kDefaultChunks;
		int32 TotalSolid = 0;

		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> RbDesc, RbOcc, RbMat, RbTable;
		uint32 DescSlots = 0, OccWords = 0, MatWords = 0, ChunkSlots = 0;
		bool bReadbackEnqueued = false;
		double StartSeconds = 0.0;

		// ---- gather: resolve real ground and pack real cover ----------------
		//
		// THE INSTRUMENT MUST RUN THE ENGINE'S BINDING (2026-08-17). columnFacts
		// is assetColumnFactsFromSample over the amplifier column AND the asset
		// channels, exactly as VoxelDetailAssetSubsystem builds it. A gate that
		// passed a channel-less facts function would be censusing the sentinel
		// world -- riparians refuse everywhere there -- which is a different
		// world and not the one the store holds.
		bool Gather(UVoxelWorldSubsystem* VoxelWorld, const FVector& AnchorUU, FString& OutWhy)
		{
			const vxc::AssetField* Field = VoxelWorld->GetAssetField();
			const vxc::Amplifier* Amp = VoxelWorld->GetWorldgenAmplifier();
			const vxc::IAssetBankSource* Banks = VoxelWorld->GetAssetBankSource();
			vxc::IAssetChannelSource* Ch = VoxelWorld->GetAssetChannelSource();
			if (Field == nullptr || Amp == nullptr || Banks == nullptr)
			{
				OutWhy = TEXT("no asset field, amplifier or bank source is installed (a run "
				              "without -VoxelAssetDir). Nothing to verify, and this is not a "
				              "statement about the store.");
				return false;
			}
			if (Ch == nullptr)
			{
				// REFUSED, NOT DEGRADED. Same rule the producer arm applies: a
				// null channel source is the sentinel world, and a volume built
				// from it is a different world.
				OutWhy = TEXT("the asset channel source is null -- that is the SENTINEL WORLD "
				              "(fail-closed water gates), and a cover volume built from it is a "
				              "different world. Refused rather than verified.");
				return false;
			}

			// NO coverVolumeInit CALL HERE, and it is not an omission. That
			// function takes an AssetManifest and vxc::AssetField exposes none,
			// so reaching it would mean routing a manifest through the world
			// subsystem for a check that ALREADY HAPPENS twice on this path:
			// resolveForCoverCompose returns empty for a pitch that does not tile
			// the world lattice, and packCoverChunk refuses the same pitch again
			// before it packs anything. A third copy of that test here would be a
			// third place for it to drift.
			const auto ColumnFacts = [Amp, Ch](int64 Vx, int64 Vy)
			{
				return vxc::assetColumnFactsFromSample(Amp->column(Vx, Vy), Ch->channelsAt(Vx, Vy));
			};

			const int64 AnchorVx =
				int64(FMath::FloorToDouble(AnchorUU.X / VoxelCoords::VoxelSizeUU));
			const int64 AnchorVy =
				int64(FMath::FloorToDouble(AnchorUU.Y / VoxelCoords::VoxelSizeUU));
			const int64 CellsPerVoxel = int64(vxc::kVoxelSizeMm) / int64(kCoverPitchMm);
			const int64 E = int64(vxc::kCoverChunkEdgeCells);

			vxc::CoverProducerCounters Counters;
			int32 GroupsSearched = 0;

			// Spiral out from the anchor in resolve-sized groups until enough
			// cover-bearing chunks are found. Ordered nearest-first so the sample
			// describes the ground under the camera rather than an arbitrary
			// patch -- section 5.2's reading rule: a single-site census is a claim
			// about that site, so at least say which site.
			for (int32 R = 0; R <= kSearchRadiusGroups && Chunks.Num() < WantChunks; ++R)
			{
				for (int32 Gy = -R; Gy <= R && Chunks.Num() < WantChunks; ++Gy)
				{
					for (int32 Gx = -R; Gx <= R && Chunks.Num() < WantChunks; ++Gx)
					{
						if (R > 0 && FMath::Abs(Gx) != R && FMath::Abs(Gy) != R)
						{
							continue;   // interior of the ring, already searched
						}
						++GroupsSearched;

						const int64 BaseVx = AnchorVx + int64(Gx) * kGroupEdgeVoxels;
						const int64 BaseVy = AnchorVy + int64(Gy) * kGroupEdgeVoxels;
						const vxc::AssetVoxelRect Rect{ BaseVx, BaseVy,
						                                BaseVx + kGroupEdgeVoxels - 1,
						                                BaseVy + kGroupEdgeVoxels - 1 };

						const std::vector<vxc::AssetInstance> Insts =
							Field->instancesForRect(Rect, ColumnFacts, /*terrainOnly*/ false);
						const std::vector<vxc::AssetField::ResolvedCoverInstance> Cover =
							Field->resolveForCoverCompose(Insts, kCoverPitchMm);
						if (Cover.empty())
						{
							continue;
						}

						// The z band the instances actually reach. Derived from
						// the SAME resolved list the pack is built from.
						int64 ZMin = TNumericLimits<int64>::Max();
						int64 ZMax = TNumericLimits<int64>::Lowest();
						for (const vxc::AssetField::ResolvedCoverInstance& CI : Cover)
						{
							const int64 Lo = CI.anchorCz + int64(CI.grid->originZ());
							const int64 Hi = Lo + int64(CI.grid->sizeZ()) - 1;
							ZMin = FMath::Min(ZMin, Lo);
							ZMax = FMath::Max(ZMax, Hi);
						}
						const int64 Cz0 = vxc::floorDiv(ZMin, E);
						const int64 Cz1 = vxc::floorDiv(ZMax, E);
						const int64 BaseCx = BaseVx * CellsPerVoxel;
						const int64 BaseCy = BaseVy * CellsPerVoxel;
						const int64 ChunksPerAxis = (kGroupEdgeVoxels * CellsPerVoxel) / E;

						for (int64 Cz = Cz0; Cz <= Cz1 && Chunks.Num() < WantChunks; ++Cz)
						{
							for (int64 Jy = 0; Jy < ChunksPerAxis && Chunks.Num() < WantChunks; ++Jy)
							{
								for (int64 Jx = 0; Jx < ChunksPerAxis && Chunks.Num() < WantChunks;
								     ++Jx)
								{
									const int64 Ccx = vxc::floorDiv(BaseCx, E) + Jx;
									const int64 Ccy = vxc::floorDiv(BaseCy, E) + Jy;
									const vxc::CoverChunkResult Packed = vxc::packCoverChunk(
										Cover, kCoverPitchMm, Ccx, Ccy, Cz, Counters);
									if (!Packed.anyCover)
									{
										continue;   // store nothing -- requirement C1
									}
									FCoverChunkUnderTest C;
									C.Coord = FIntVector(int32(Ccx), int32(Ccy), int32(Cz));
									C.Cpu = Packed.pack;
									for (int32 X = 0; X < int32(vxc::kCoverChunkEdgeCells); ++X)
									{
										for (int32 Y = 0; Y < int32(vxc::kCoverChunkEdgeCells); ++Y)
										{
											for (int32 Z = 0;
											     Z < int32(vxc::kCoverChunkEdgeCells); ++Z)
											{
												if (vxc::decodeChunkVoxelCanonical(C.Cpu, X, Y, Z)
												    != vxc::MAT_AIR)
												{
													++C.SolidVoxels;
												}
											}
										}
									}
									TotalSolid += C.SolidVoxels;
									Chunks.Add(MoveTemp(C));
								}
							}
						}
					}
				}
			}

			UE_LOG(LogVoxelCoverVerify, Log,
			       TEXT("voxel.Cover.VerifyStore: searched %d groups around voxel (%lld,%lld); "
			            "producer funnel attempted %llu -> resolved %llu -> packed %llu; kept %d "
			            "cover chunks holding %d solid cells at pitch %u mm."),
			       GroupsSearched, (long long)AnchorVx, (long long)AnchorVy,
			       (unsigned long long)Counters.attempted(),
			       (unsigned long long)Counters.resolved(),
			       (unsigned long long)Counters.packed(), Chunks.Num(), TotalSolid,
			       kCoverPitchMm);
			return true;
		}

		// ---- publish into the private pool ----------------------------------
		void Publish()
		{
			// Sized to the run so the guard band covers every byte this run could
			// have touched. The arenas are created zero-filled and this gate never
			// frees, so an out-of-range write has nowhere to hide -- the same
			// property voxel.GPU.VerifyBrickPack relies on.
			FVoxelBrickPoolConfig Config;
			Config.ChunkCapacity = uint32(Chunks.Num()) + 4u;
			Config.OccWordCapacity = uint32(Chunks.Num() + 1) * 64u * 16u + 4096u;
			Config.MatWordCapacity = uint32(Chunks.Num() + 1) * 64u * 132u + 4096u;
			Pool.Init(Config);

			for (FCoverChunkUnderTest& C : Chunks)
			{
				const FVoxelBrickCpuPackRef Pack = VoxelBrickCpuPackFromCore(
					C.Cpu, int64(C.Coord.X) * vxc::kCoverChunkEdgeCells,
					int64(C.Coord.Y) * vxc::kCoverChunkEdgeCells,
					int64(C.Coord.Z) * vxc::kCoverChunkEdgeCells);

				FVoxelBrickChunkKey Key;
				Key.X = C.Coord.X;
				Key.Y = C.Coord.Y;
				Key.Z = C.Coord.Z;
				Key.Level = FVoxelBrickPool::kCoverLevel;
				if (Pool.AddChunkFromCpu(Pack, Key, FVoxelBrickChunkShading::Neutral()) == INDEX_NONE)
				{
					continue;   // refused; GetAllocFailures() has moved
				}
				// WHERE IT ACTUALLY LANDED, asked of the pool rather than
				// assumed. Format section 6b: a base compared against a chunk
				// offset reports a mismatch that is not one, and it looks exactly
				// like the format being wrong.
				C.bResident = Pool.DebugGetResidentChunk(Key, C.Res);
			}
			Pool.Flush();
		}

		void EnqueueReadback()
		{
			const FVoxelBrickPoolBuffersRef Buffers = Pool.DebugGetBuffers();
			if (!Buffers.IsValid() || !Buffers->IsValid())
			{
				return;
			}
			DescSlots = Buffers->DescSlots;
			OccWords = Buffers->OccWords;
			MatWords = Buffers->MatWords;
			ChunkSlots = Buffers->ChunkSlots;

			RbDesc = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(
				TEXT("Voxel.CoverVerify.Desc"));
			RbOcc = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(
				TEXT("Voxel.CoverVerify.Occ"));
			RbMat = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(
				TEXT("Voxel.CoverVerify.Mat"));
			RbTable = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(
				TEXT("Voxel.CoverVerify.Table"));

			TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> D = RbDesc, O = RbOcc,
			                                                       M = RbMat, T = RbTable;
			const uint32 InDesc = DescSlots, InOcc = OccWords, InMat = MatWords,
			             InTable = ChunkSlots;
			ENQUEUE_RENDER_COMMAND(VoxelCoverVerifyReadback)(
				[Buffers, D, O, M, T, InDesc, InOcc, InMat, InTable]
				(FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FRDGBufferRef Desc = GraphBuilder.RegisterExternalBuffer(
					Buffers->DescPooled, TEXT("CoverVerify.Desc"));
				FRDGBufferRef Occ = GraphBuilder.RegisterExternalBuffer(
					Buffers->OccPooled, TEXT("CoverVerify.Occ"));
				FRDGBufferRef Mat = GraphBuilder.RegisterExternalBuffer(
					Buffers->MatPooled, TEXT("CoverVerify.Mat"));
				FRDGBufferRef Table = GraphBuilder.RegisterExternalBuffer(
					Buffers->ChunkTablePooled, TEXT("CoverVerify.Table"));
				AddEnqueueCopyPass(GraphBuilder, D.Get(), Desc, InDesc * 8u);
				AddEnqueueCopyPass(GraphBuilder, O.Get(), Occ, InOcc * 4u);
				AddEnqueueCopyPass(GraphBuilder, M.Get(), Mat, InMat * 4u);
				AddEnqueueCopyPass(GraphBuilder, T.Get(), Table,
				                   InTable * uint32(FVoxelBrickPool::kChunkRecordDwords) * 4u);
				GraphBuilder.Execute();
			});
			bReadbackEnqueued = true;
		}

		static void Harvest(FRHIGPUBufferReadback& Rb, uint32 Bytes, TArray<uint32>& Out)
		{
			Out.SetNumZeroed(int32(Bytes / 4u));
			if (const void* Src = Rb.Lock(Bytes))
			{
				FMemory::Memcpy(Out.GetData(), Src, Bytes);
				Rb.Unlock();
			}
		}

		bool Ready() const
		{
			return bReadbackEnqueued && RbDesc.IsValid() && RbDesc->IsReady() && RbOcc->IsReady()
			    && RbMat->IsReady() && RbTable->IsReady();
		}

		void Report()
		{
			TArray<uint32> PoolDesc, PoolOcc, PoolMat, PoolTable;
			Harvest(*RbDesc, DescSlots * 8u, PoolDesc);
			Harvest(*RbOcc, OccWords * 4u, PoolOcc);
			Harvest(*RbMat, MatWords * 4u, PoolMat);
			Harvest(*RbTable, ChunkSlots * uint32(FVoxelBrickPool::kChunkRecordDwords) * 4u,
			        PoolTable);

			TArray<const FCoverChunkUnderTest*> Live;
			for (const FCoverChunkUnderTest& C : Chunks)
			{
				if (C.bResident)
				{
					Live.Add(&C);
				}
			}

			// ---- THE ABSTAIN FLOORS, CHECKED BEFORE ANY ARM RUNS ------------
			if (Live.Num() < kMinChunks || TotalSolid < kMinSolidVoxels)
			{
				UE_LOG(LogVoxelCoverVerify, Warning,
				       TEXT("voxel.Cover.VerifyStore: NO VERDICT -- %d resident cover chunks "
				            "(floor %d) holding %d solid cells (floor %d). The producer ran and "
				            "found too little cover on this ground to prove anything about the "
				            "store. THIS IS NOT A PASS. Move to vegetated ground and re-run; the "
				            "alpine census site returned 10.5 MiB and three species, none of them "
				            "ground cover."),
				       Live.Num(), kMinChunks, TotalSolid, kMinSolidVoxels);
				return;
			}

			// ---- ARM 1: DIRECT ----------------------------------------------
			int32 DirectFail = 0, RecordFail = 0;
			for (const FCoverChunkUnderTest* C : Live)
			{
				DirectFail += CompareChunkBytes(C->Cpu, C->Res.OccBase, C->Res.MatBase, PoolDesc,
				                                C->Res.BrickBase, PoolOcc, C->Res.OccBase, PoolMat,
				                                C->Res.MatBase);
				RecordFail += CompareChunkRecord(*C, PoolTable);
			}

			// ---- ARM 2: REBASE CONTROL -- must DISAGREE ---------------------
			//
			// The same bytes against the reference with NO bases folded in. If
			// this agrees, the rebase is a no-op and the direct arm proved
			// nothing. Chunks that landed at base 0 legitimately agree, so the
			// arm is counted over chunks whose bases are actually non-zero.
			int32 RebaseEligible = 0, RebaseSilent = 0;
			for (const FCoverChunkUnderTest* C : Live)
			{
				if (C->Res.OccBase == 0 && C->Res.MatBase == 0)
				{
					continue;
				}
				++RebaseEligible;
				if (CompareChunkBytes(C->Cpu, 0, 0, PoolDesc, C->Res.BrickBase, PoolOcc,
				                      C->Res.OccBase, PoolMat, C->Res.MatBase) == 0)
				{
					++RebaseSilent;
				}
			}

			// ---- ARM 3: DISPLACED CONTROL -- must DISAGREE ------------------
			int32 DisplacedSilent = 0;
			for (int32 I = 0; I < Live.Num(); ++I)
			{
				const FCoverChunkUnderTest* Mine = Live[I];
				const FCoverChunkUnderTest* Other = Live[(I + 1) % Live.Num()];
				if (CompareChunkBytes(Other->Cpu, Mine->Res.OccBase, Mine->Res.MatBase, PoolDesc,
				                      Mine->Res.BrickBase, PoolOcc, Mine->Res.OccBase, PoolMat,
				                      Mine->Res.MatBase) == 0)
				{
					++DisplacedSilent;
				}
			}

			// ---- ARM 4: BITFLIP -- must be DETECTED -------------------------
			//
			// One bit, in a COPY of the reference, compared through the same
			// comparator. Proves single-bit sensitivity on this run.
			bool bBitflipDetected = false;
			{
				const FCoverChunkUnderTest* C = Live[0];
				vxc::ChunkBrickPack Mutated = C->Cpu;
				if (!Mutated.occ.empty())
				{
					Mutated.occ[0] ^= 1u;
				}
				else if (!Mutated.mat.empty())
				{
					Mutated.mat[0] ^= 1u;
				}
				else
				{
					Mutated.descs[0].MatWord ^= 1u;
				}
				bBitflipDetected =
					CompareChunkBytes(Mutated, C->Res.OccBase, C->Res.MatBase, PoolDesc,
					                  C->Res.BrickBase, PoolOcc, C->Res.OccBase, PoolMat,
					                  C->Res.MatBase) > 0;
			}

			// ---- ARM 5: GUARD BAND ------------------------------------------
			//
			// Built from the allocations the POOL handed out, not from what the
			// writes were asked to do -- so a write that landed outside its own
			// allocation shows up even if its bytes are perfect.
			int32 GuardFail = 0;
			{
				TArray<bool> OccLive, MatLive, DescLive;
				OccLive.Init(false, int32(OccWords));
				MatLive.Init(false, int32(MatWords));
				DescLive.Init(false, int32(DescSlots));
				TSet<uint32> LiveSlots;
				for (const FCoverChunkUnderTest* C : Live)
				{
					LiveSlots.Add(C->Res.ChunkSlot);
					for (uint32 W = 0; W < FVoxelBrickPool::kBricksPerChunk; ++W)
					{
						const int32 S = int32(C->Res.BrickBase + W);
						if (DescLive.IsValidIndex(S)) { DescLive[S] = true; }
					}
					for (uint32 W = 0; W < C->Res.OccWords; ++W)
					{
						const int32 I = int32(C->Res.OccBase + W);
						if (OccLive.IsValidIndex(I)) { OccLive[I] = true; }
					}
					for (uint32 W = 0; W < C->Res.MatWords; ++W)
					{
						const int32 I = int32(C->Res.MatBase + W);
						if (MatLive.IsValidIndex(I)) { MatLive[I] = true; }
					}
				}
				for (int32 I = 0; I < OccLive.Num(); ++I)
				{
					if (!OccLive[I] && PoolOcc.IsValidIndex(I) && PoolOcc[I] != 0u) { ++GuardFail; }
				}
				for (int32 I = 0; I < MatLive.Num(); ++I)
				{
					if (!MatLive[I] && PoolMat.IsValidIndex(I) && PoolMat[I] != 0u) { ++GuardFail; }
				}
				for (int32 S = 0; S < DescLive.Num(); ++S)
				{
					if (DescLive[S] && PoolDesc.IsValidIndex(S * 2 + 1)) { continue; }
					if (!DescLive[S] && PoolDesc.IsValidIndex(S * 2 + 1)
					    && (PoolDesc[S * 2 + 0] != 0u || PoolDesc[S * 2 + 1] != 0u))
					{
						++GuardFail;
					}
				}
				for (uint32 S = 0; S < ChunkSlots; ++S)
				{
					if (LiveSlots.Contains(S)) { continue; }
					for (int32 W = 0; W < FVoxelBrickPool::kChunkRecordDwords; ++W)
					{
						const int32 I = int32(S) * FVoxelBrickPool::kChunkRecordDwords + W;
						if (PoolTable.IsValidIndex(I) && PoolTable[I] != 0u) { ++GuardFail; }
					}
				}
			}

			// ---- THE VERDICT -------------------------------------------------
			//
			// THE CONTROL ARMS ARE REPORTED SEPARATELY AND FIRST. A silent control
			// means the gate is not measuring the bytes, which is a statement
			// about the instrument -- reporting it as a cover failure would send
			// somebody to debug the store.
			const bool bInstrumentOk =
				(RebaseEligible == 0 || RebaseSilent == 0) && DisplacedSilent == 0
				&& bBitflipDetected;
			if (!bInstrumentOk)
			{
				UE_LOG(LogVoxelCoverVerify, Error,
				       TEXT("voxel.Cover.VerifyStore: INSTRUMENT BROKEN -- NO VERDICT ON COVER. "
				            "rebaseEligible=%d rebaseSilent=%d displacedSilent=%d/%d "
				            "bitflipDetected=%d. A control arm that agrees means this gate cannot "
				            "tell correct bytes from deliberately wrong ones, so its direct arm "
				            "(fail=%d) carries NO information. Fix the gate before reading it."),
				       RebaseEligible, RebaseSilent, DisplacedSilent, Live.Num(),
				       bBitflipDetected ? 1 : 0, DirectFail);
				return;
			}

			const bool bPass = (DirectFail == 0 && RecordFail == 0 && GuardFail == 0);
			// TWO SITES, NOT A TERNARY: UE_LOG's verbosity is a compile-time
			// token and not a value.
			if (bPass)
			{
				UE_LOG(LogVoxelCoverVerify, Log,
				       TEXT("voxel.Cover.VerifyStore: PASS -- %d/%d cover chunks byte-identical to "
				            "vxc::packCoverChunk over the same resolved list. directFail=0 "
				            "recordFail=0 guardFail=0; %d solid cover cells. CONTROLS HELD: rebase "
				            "disagreed on %d/%d, displaced disagreed on %d/%d, one-bit flip "
				            "detected. Tolerance is exactly zero -- both sides are integer voxels."),
				       Live.Num(), Chunks.Num(), TotalSolid, RebaseEligible - RebaseSilent,
				       RebaseEligible, Live.Num() - DisplacedSilent, Live.Num());
			}
			else
			{
				UE_LOG(LogVoxelCoverVerify, Error,
				       TEXT("voxel.Cover.VerifyStore: FAIL -- %d/%d cover chunks checked. "
				            "directFail=%d recordFail=%d guardFail=%d; %d solid cover cells. The "
				            "CONTROLS HELD (rebase %d/%d, displaced %d/%d, bit flip detected), so "
				            "this is a statement about the STORE and not about the gate."),
				       Live.Num(), Chunks.Num(), DirectFail, RecordFail, GuardFail, TotalSolid,
				       RebaseEligible - RebaseSilent, RebaseEligible,
				       Live.Num() - DisplacedSilent, Live.Num());
			}
		}
	};

	TSharedPtr<FCoverVerifyRun, ESPMode::ThreadSafe> GCoverVerifyRun;

	void CoverVerifyStoreCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (GCoverVerifyRun.IsValid())
		{
			UE_LOG(LogVoxelCoverVerify, Warning,
			       TEXT("voxel.Cover.VerifyStore: a run is already in flight."));
			return;
		}
		if (World == nullptr)
		{
			UE_LOG(LogVoxelCoverVerify, Error,
			       TEXT("voxel.Cover.VerifyStore: no game world -- this gate needs a live world "
			            "for the asset field and the amplifier."));
			return;
		}
		UVoxelWorldSubsystem* VoxelWorld = World->GetSubsystem<UVoxelWorldSubsystem>();
		if (VoxelWorld == nullptr)
		{
			UE_LOG(LogVoxelCoverVerify, Error,
			       TEXT("voxel.Cover.VerifyStore: no UVoxelWorldSubsystem."));
			return;
		}

		FVector Anchor = FVector::ZeroVector;
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				Anchor = Pawn->GetActorLocation();
			}
		}

		TSharedPtr<FCoverVerifyRun, ESPMode::ThreadSafe> Run =
			MakeShared<FCoverVerifyRun, ESPMode::ThreadSafe>();
		if (Args.Num() > 0)
		{
			Run->WantChunks = FMath::Clamp(FCString::Atoi(*Args[0]), kMinChunks, 64);
		}
		Run->StartSeconds = FPlatformTime::Seconds();

		FString Why;
		if (!Run->Gather(VoxelWorld, Anchor, Why))
		{
			UE_LOG(LogVoxelCoverVerify, Warning,
			       TEXT("voxel.Cover.VerifyStore: NO VERDICT -- %s"), *Why);
			return;
		}
		if (Run->Chunks.Num() == 0)
		{
			UE_LOG(LogVoxelCoverVerify, Warning,
			       TEXT("voxel.Cover.VerifyStore: NO VERDICT -- the producer ran over %d groups "
			            "and packed no cover chunk at all. That is a real answer about this "
			            "ground, not a pass and not a defect."),
			       kSearchRadiusGroups);
			return;
		}
		Run->Publish();
		GCoverVerifyRun = Run;

		// SLACK BEFORE THE READBACK, AND IT MUST BE MORE THAN ONE TICK.
		//
		// The pool's arenas are created lazily by the first Flush -- but Flush
		// only ENQUEUES a render command, and the RHI buffers inside
		// FVoxelBrickPoolBuffers are created when THE RENDER THREAD runs it.
		// The render thread is routinely a frame or two behind the game thread,
		// so "one game-thread tick" is not a guarantee that the command has
		// executed; it is a guess that happened to be wrong every time this gate
		// was run (three legs, three "no GPU buffers", no verdict ever).
		//
		// So retry for a bounded window instead of erroring on the first miss.
		// THE REFUSAL IS KEPT AND CAN STILL FIRE: a pool that genuinely never
		// creates buffers still reports, after the window, with the elapsed time
		// in the message so a real absence reads differently from a race. What is
		// removed is only the assumption that one tick is enough.
		//
		// The window is deliberately shorter than the 30 s readback timeout
		// below, so the two failures stay distinguishable: this one means the
		// buffers never appeared, that one means the copy never landed.
		constexpr double kBuffersWaitSeconds = 5.0;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
		{
			TSharedPtr<FCoverVerifyRun, ESPMode::ThreadSafe> R = GCoverVerifyRun;
			if (!R.IsValid())
			{
				return false;
			}
			if (!R->bReadbackEnqueued)
			{
				R->EnqueueReadback();
				if (!R->bReadbackEnqueued)
				{
					const double Waited = FPlatformTime::Seconds() - R->StartSeconds;
					if (Waited < kBuffersWaitSeconds)
					{
						// Not an answer yet. Come back next tick.
						return true;
					}
					UE_LOG(LogVoxelCoverVerify, Error,
					       TEXT("voxel.Cover.VerifyStore: the private pool has no GPU buffers after "
					            "%.1f s -- nothing was read back and nothing is verified. This is "
					            "now a real absence and not the one-tick race it used to be: the "
					            "pool was given %d chunk(s) and its arenas still never appeared."),
					       Waited, R->Chunks.Num());
					GCoverVerifyRun.Reset();
					return false;
				}
				return true;
			}
			if (R->Ready())
			{
				R->Report();
				GCoverVerifyRun.Reset();
				return false;
			}
			if (FPlatformTime::Seconds() - R->StartSeconds > 30.0)
			{
				UE_LOG(LogVoxelCoverVerify, Error,
				       TEXT("voxel.Cover.VerifyStore: the readback never landed. NO VERDICT."));
				GCoverVerifyRun.Reset();
				return false;
			}
			return true;
		}), 0.0f);
	}

	FAutoConsoleCommandWithWorldAndArgs GCoverVerifyStoreCmd(
		TEXT("voxel.Cover.VerifyStore"),
		TEXT("BYTE GATE ON THE COVER STORE. Packs K real cover chunks with vxc::packCoverChunk, "
		     "publishes them into a PRIVATE FVoxelBrickPool at level 7, reads the arenas back and "
		     "compares byte for byte. Four arms every run: DIRECT (must agree), REBASE and "
		     "DISPLACED controls (must DISAGREE), and a one-bit flip that must be detected -- a "
		     "gate that cannot tell correct bytes from wrong ones is not a gate. Plus a guard band "
		     "over every unallocated dword. ABSTAINS with NO VERDICT below 4 chunks or 256 solid "
		     "cover cells rather than calling an empty world a pass. Usage: [K=8]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&CoverVerifyStoreCommand));
} // namespace
