// Tests for the GPU geometry pool suballocator (ADR-0006, G2).
//
// This allocator decides which chunk's geometry lives where in one shared GPU
// buffer. A bug here does not crash — it hands two chunks the same range and
// the terrain flickers between two shapes, which is a miserable thing to debug
// from a screenshot. So it gets real coverage, including the randomised
// soak that catches the ordering mistakes hand-written cases miss.
//
// Run headlessly:
//   UnrealEditor-Cmd.exe VoxelEarth.uproject -unattended -nullrhi -nop4 \
//     -ExecCmds="Automation RunTests VoxelEarth.GpuPool; Quit"

#include "VoxelBrickPool.h"        // P2 residency, which reuses this suballocator
#include "VoxelGpuGeometryPool.h"
#include "VoxelGpuPoolComponent.h" // S1-1 dirty-range algebra (DebugMark/UnmarkQuadsDirty)
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// EAutomationTestFlags is a strongly-typed enum class in 5.8, not an int
	// bitmask, so this must keep the enum type all the way through.
	constexpr EAutomationTestFlags kTestFlags = EAutomationTestFlags::EditorContext
	                                          | EAutomationTestFlags::ClientContext
	                                          | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGpuPoolBasicTest,
	"VoxelEarth.GpuPool.Basic", kTestFlags)

bool FVoxelGpuPoolBasicTest::RunTest(const FString& Parameters)
{
	FVoxelGpuGeometryPool Pool;
	Pool.Init(1000);

	TestEqual(TEXT("capacity"), Pool.GetCapacityQuads(), 1000u);
	TestEqual(TEXT("starts empty"), Pool.GetUsedQuads(), 0u);
	TestEqual(TEXT("starts unfragmented"), Pool.GetFreeRunCount(), 1);
	TestEqual(TEXT("high water starts at zero"), Pool.GetHighWaterMark(), 0u);

	const FVoxelGpuPoolAllocation A = Pool.Alloc(100);
	TestTrue(TEXT("first alloc succeeds"), A.IsValid());
	TestEqual(TEXT("first alloc starts at zero"), A.Offset, 0u);
	TestEqual(TEXT("used after first alloc"), Pool.GetUsedQuads(), 100u);
	TestEqual(TEXT("high water after first alloc"), Pool.GetHighWaterMark(), 100u);

	const FVoxelGpuPoolAllocation B = Pool.Alloc(250);
	TestTrue(TEXT("second alloc succeeds"), B.IsValid());
	TestEqual(TEXT("second alloc packs against the first"), B.Offset, 100u);
	TestEqual(TEXT("used after second alloc"), Pool.GetUsedQuads(), 350u);

	// A zero-quad chunk is normal — buried and empty chunks mesh to nothing.
	// It must succeed rather than read as an out-of-memory failure.
	const FVoxelGpuPoolAllocation Empty = Pool.Alloc(0);
	TestTrue(TEXT("zero-quad alloc is valid, not a failure"), Empty.IsValid());
	TestEqual(TEXT("zero-quad alloc consumes nothing"), Pool.GetUsedQuads(), 350u);

	Pool.Free(A);
	TestEqual(TEXT("used after free"), Pool.GetUsedQuads(), 250u);

	FString Error;
	TestTrue(*FString::Printf(TEXT("invariants hold: %s"), *Error), Pool.CheckInvariants(Error));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGpuPoolCoalesceTest,
	"VoxelEarth.GpuPool.Coalesce", kTestFlags)

bool FVoxelGpuPoolCoalesceTest::RunTest(const FString& Parameters)
{
	FVoxelGpuGeometryPool Pool;
	Pool.Init(300);

	const FVoxelGpuPoolAllocation A = Pool.Alloc(100);
	const FVoxelGpuPoolAllocation B = Pool.Alloc(100);
	const FVoxelGpuPoolAllocation C = Pool.Alloc(100);
	TestTrue(TEXT("pool fills exactly"), A.IsValid() && B.IsValid() && C.IsValid());
	TestEqual(TEXT("full pool has no free runs"), Pool.GetFreeRunCount(), 0);
	TestFalse(TEXT("a full pool refuses"), Pool.Alloc(1).IsValid());

	// Free the outer two first, leaving a hole in the middle. Nothing should
	// coalesce yet — the three runs are not adjacent.
	Pool.Free(A);
	Pool.Free(C);
	TestEqual(TEXT("two separate holes"), Pool.GetFreeRunCount(), 2);
	TestEqual(TEXT("200 quads free but only 100 contiguous"), Pool.GetFreeQuads(), 200u);
	TestEqual(TEXT("largest run is one hole"), Pool.GetLargestFreeRun(), 100u);
	TestFalse(TEXT("fragmentation refuses a 150 alloc despite 200 free"),
	          Pool.Alloc(150).IsValid());

	// Freeing the middle must merge all three into one run — this is the case
	// that separates a working coalescer from one that only merges forward.
	Pool.Free(B);
	TestEqual(TEXT("all three merge into one run"), Pool.GetFreeRunCount(), 1);
	TestEqual(TEXT("whole pool free again"), Pool.GetFreeQuads(), 300u);
	TestEqual(TEXT("largest run is the whole pool"), Pool.GetLargestFreeRun(), 300u);
	TestTrue(TEXT("a full-size alloc now succeeds"), Pool.Alloc(300).IsValid());

	FString Error;
	TestTrue(*FString::Printf(TEXT("invariants hold: %s"), *Error), Pool.CheckInvariants(Error));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGpuPoolSoakTest,
	"VoxelEarth.GpuPool.Soak", kTestFlags)

bool FVoxelGpuPoolSoakTest::RunTest(const FString& Parameters)
{
	// Fixed seed: a failure has to be reproducible, and an allocator bug that
	// only shows up on some runs is worse than no test at all.
	FRandomStream Rand(20260725);

	FVoxelGpuGeometryPool Pool;
	Pool.Init(64 * 1024);

	TArray<FVoxelGpuPoolAllocation> Live;
	int32 AllocCount = 0;
	int32 FailCount = 0;

	for (int32 Step = 0; Step < 20000; ++Step)
	{
		// Bias toward allocating while the pool is empty and toward freeing
		// once it is busy, so the run spends its time in the interesting
		// middle rather than pinned at either extreme.
		const bool bWantAlloc = Live.IsEmpty() || Rand.FRand() < 0.55f;

		if (bWantAlloc)
		{
			// Chunk quad counts in the real world sit in the 600-2400 band
			// (docs/gpu-g0-sizing.md), with zero-quad chunks common.
			const uint32 Size = (Rand.FRand() < 0.1f)
				? 0u
				: uint32(Rand.RandRange(600, 2400));

			const FVoxelGpuPoolAllocation Alloc = Pool.Alloc(Size);
			if (Alloc.IsValid())
			{
				if (Size > 0)
				{
					// Every live range must be disjoint from every other one.
					// This is the property the whole class exists to provide.
					for (const FVoxelGpuPoolAllocation& Other : Live)
					{
						const bool bOverlaps = Alloc.Offset < Other.Offset + Other.NumQuads
						                    && Other.Offset < Alloc.Offset + Alloc.NumQuads;
						if (bOverlaps)
						{
							AddError(FString::Printf(
								TEXT("step %d: allocation [%u, %u) overlaps live [%u, %u)"),
								Step, Alloc.Offset, Alloc.Offset + Alloc.NumQuads,
								Other.Offset, Other.Offset + Other.NumQuads));
							return false;
						}
					}
					Live.Add(Alloc);
				}
				++AllocCount;
			}
			else
			{
				++FailCount;
			}
		}
		else
		{
			const int32 Index = Rand.RandRange(0, Live.Num() - 1);
			Pool.Free(Live[Index]);
			Live.RemoveAtSwap(Index, EAllowShrinking::No);
		}

		if ((Step % 500) == 0)
		{
			FString Error;
			if (!Pool.CheckInvariants(Error))
			{
				AddError(FString::Printf(TEXT("step %d: %s"), Step, *Error));
				return false;
			}
		}
	}

	// Draining everything must return the pool to pristine — one free run
	// covering the whole capacity. If coalescing ever misses a case, this is
	// where it shows up as leftover slivers.
	for (const FVoxelGpuPoolAllocation& Alloc : Live)
	{
		Pool.Free(Alloc);
	}
	Live.Reset();

	TestEqual(TEXT("fully drained pool is empty"), Pool.GetUsedQuads(), 0u);
	TestEqual(TEXT("fully drained pool has exactly one free run"), Pool.GetFreeRunCount(), 1);
	TestEqual(TEXT("fully drained pool is whole again"),
	          Pool.GetLargestFreeRun(), Pool.GetCapacityQuads());

	FString Error;
	TestTrue(*FString::Printf(TEXT("invariants hold: %s"), *Error), Pool.CheckInvariants(Error));

	AddInfo(FString::Printf(TEXT("soak: %d allocations, %d refusals from fragmentation"),
	                        AllocCount, FailCount));
	return true;
}

// ============================================================================
// S1-1: the dirty-range interval algebra.
//
// WHY THIS EXISTS RATHER THAN A LEG. UnmarkQuadsDirty guards the
// free-then-reallocate-within-one-frame race that batching publication creates
// (see its declaration). Across four full flight legs it fired ZERO times --
// the tick ordering puts most frees after allocs, so the case is rare but NOT
// unreachable: ApplyMeshResult's zero-quad branch frees inside DrainResults and
// a later result in the same drain loop can AddChunkFromGpu over that range.
//
// So the guard is load-bearing for a case that has never been observed, which
// is the worst state to ship in -- ground rule 13. If the subtract is wrong the
// symptom is stale hidden ids written over fresh GPU geometry: INVISIBLE
// TERRAIN THAT REPORTS AS LOADED, with no counter moving and nothing in a
// screenshot to see.
//
// The algebra is pure list manipulation, so it is tested directly instead of
// waiting for a leg to happen to hit it. The SPLIT case is the one that matters
// most: getting it wrong leaves either a gap that fails to upload real CPU
// content, or an interval that survives over GPU-written quads.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGpuPoolDirtyRangeTest,
	"VoxelEarth.GpuPool.DirtyRanges", kTestFlags)

bool FVoxelGpuPoolDirtyRangeTest::RunTest(const FString& Parameters)
{
	using FRanges = TArray<TPair<uint32, uint32>>;

	// Transient, never registered, InitPool never called: these two methods only
	// touch DirtyQuadRanges.
	UVoxelGpuPoolComponent* Pool = NewObject<UVoxelGpuPoolComponent>();
	if (!TestNotNull(TEXT("component constructed"), Pool))
	{
		return false;
	}

	const auto Reset = [Pool]()
	{
		// Subtracting everything is the only public way back to empty, and it
		// doubles as a check that a full-cover subtract really does clear.
		Pool->DebugUnmarkQuadsDirty(0, TNumericLimits<uint32>::Max());
	};

	const auto Expect = [this, Pool](const TCHAR* What, const FRanges& Want)
	{
		const FRanges Got = Pool->DebugGetDirtyRanges();
		if (!TestEqual(*FString::Printf(TEXT("%s: range count"), What), Got.Num(), Want.Num()))
		{
			return;
		}
		for (int32 I = 0; I < Want.Num(); ++I)
		{
			TestEqual(*FString::Printf(TEXT("%s: [%d].First"), What, I), Got[I].Key, Want[I].Key);
			TestEqual(*FString::Printf(TEXT("%s: [%d].Last"), What, I), Got[I].Value, Want[I].Value);
		}
	};

	// --- the five cases the subtract has to get right ----------------------

	// 1. Disjoint -- must not touch anything.
	Reset();
	Pool->DebugMarkQuadsDirty(100, 50);                 // [100,149]
	Pool->DebugUnmarkQuadsDirty(200, 10);               // [200,209]
	Expect(TEXT("disjoint above"), FRanges{ {100, 149} });
	Pool->DebugUnmarkQuadsDirty(10, 10);                // [10,19]
	Expect(TEXT("disjoint below"), FRanges{ {100, 149} });

	// 2. Fully covered -- the interval disappears entirely.
	Reset();
	Pool->DebugMarkQuadsDirty(100, 50);
	Pool->DebugUnmarkQuadsDirty(100, 50);
	Expect(TEXT("exact cover"), FRanges{});

	Reset();
	Pool->DebugMarkQuadsDirty(100, 50);
	Pool->DebugUnmarkQuadsDirty(90, 100);               // [90,189] strictly wider
	Expect(TEXT("wider cover"), FRanges{});

	// 3. Head survives -- overlap runs off the top.
	Reset();
	Pool->DebugMarkQuadsDirty(100, 50);                 // [100,149]
	Pool->DebugUnmarkQuadsDirty(120, 100);              // [120,219]
	Expect(TEXT("keep head"), FRanges{ {100, 119} });

	// 4. Tail survives -- overlap runs off the bottom.
	Reset();
	Pool->DebugMarkQuadsDirty(100, 50);                 // [100,149]
	Pool->DebugUnmarkQuadsDirty(50, 71);                // [50,120]
	Expect(TEXT("keep tail"), FRanges{ {121, 149} });

	// 5. THE SPLIT. Removed range strictly inside: two intervals must come out,
	// in ascending order, with the removed quads in neither.
	Reset();
	Pool->DebugMarkQuadsDirty(100, 50);                 // [100,149]
	Pool->DebugUnmarkQuadsDirty(120, 10);               // [120,129]
	Expect(TEXT("split"), FRanges{ {100, 119}, {130, 149} });

	// A single-quad hole is still a split -- the off-by-one that would merge it
	// away or drop a neighbour is exactly what this catches.
	Reset();
	Pool->DebugMarkQuadsDirty(100, 50);
	Pool->DebugUnmarkQuadsDirty(125, 1);                // [125,125]
	Expect(TEXT("single-quad split"), FRanges{ {100, 124}, {126, 149} });

	// Boundary quads: removing exactly the first or last quad must not split.
	Reset();
	Pool->DebugMarkQuadsDirty(100, 50);
	Pool->DebugUnmarkQuadsDirty(100, 1);
	Expect(TEXT("first quad only"), FRanges{ {101, 149} });

	Reset();
	Pool->DebugMarkQuadsDirty(100, 50);
	Pool->DebugUnmarkQuadsDirty(149, 1);
	Expect(TEXT("last quad only"), FRanges{ {100, 148} });

	// --- across several intervals, which is the real shape ------------------
	//
	// A batched tick holds one interval per CPU-written chunk, and a GPU
	// allocation can land across any of them. Gap 0 means these stay separate
	// (see GVoxelPoolDirtyMergeGap -- that default is load-bearing for D1).
	Reset();
	Pool->DebugMarkQuadsDirty(100, 20);                 // [100,119]
	Pool->DebugMarkQuadsDirty(200, 20);                 // [200,219]
	Pool->DebugMarkQuadsDirty(300, 20);                 // [300,319]
	Expect(TEXT("three separate"), FRanges{ {100, 119}, {200, 219}, {300, 319} });

	// Spanning the middle one entirely and clipping both neighbours.
	Pool->DebugUnmarkQuadsDirty(110, 200);              // [110,309]
	Expect(TEXT("span middle, clip both"), FRanges{ {100, 109}, {310, 319} });

	// Count of zero is a no-op, not a degenerate range. (Count 0 would compute
	// Last = First - 1 and underflow if it were not handled up front.)
	Reset();
	Pool->DebugMarkQuadsDirty(100, 50);
	Pool->DebugUnmarkQuadsDirty(120, 0);
	Expect(TEXT("zero count is a no-op"), FRanges{ {100, 149} });

	// Subtracting from an empty list must not touch anything either.
	Reset();
	Pool->DebugUnmarkQuadsDirty(100, 50);
	Expect(TEXT("subtract from empty"), FRanges{});

	// --- the actual hazard, in the order it would occur ---------------------
	//
	// RemoveChunk frees a range and stamps hidden ids over it (MarkQuadsDirty),
	// then AddChunkFromGpu is handed that same range back by the first-fit
	// allocator in the same batch and subtracts it. What must remain is the
	// OTHER chunks' dirty content and nothing covering the GPU-written range.
	Reset();
	Pool->DebugMarkQuadsDirty(0, 100);                  // another chunk, CPU-written
	Pool->DebugMarkQuadsDirty(100, 50);                 // the chunk being freed
	Pool->DebugMarkQuadsDirty(200, 100);                // another chunk, CPU-written
	Pool->DebugUnmarkQuadsDirty(100, 50);               // re-issued to a GPU-meshed chunk
	Expect(TEXT("hazard: GPU range excluded, neighbours intact"),
	       FRanges{ {0, 99}, {200, 299} });

	// And the counter must have moved, or the fix is not wired to anything.
	TestTrue(TEXT("dirtyOverlapsResolved recorded the subtracts"),
	         Pool->GetDirtyOverlapsResolved() > 0);
	TestTrue(TEXT("dirtyOverlapQuadsResolved recorded the quads"),
	         Pool->GetDirtyOverlapQuadsResolved() > 0);

	return true;
}

// ---------------------------------------------------------------------------
// P2: FVoxelBrickPool's bookkeeping
// ---------------------------------------------------------------------------
//
// The suballocation itself is covered above -- the brick pool reuses this exact
// class, counting dwords and descriptor slots instead of quads. What is NEW and
// worth covering is the bookkeeping built on top of it, all of which is pure CPU
// state and needs no RHI:
//
//   * ChunkSlot = BrickBase / 64 is only sound because EVERY descriptor
//     allocation is exactly 64 slots. If that alignment ever breaks, two chunks
//     share a record and the marcher reads one chunk's bricks at another's
//     origin -- a wrong world with no error anywhere.
//   * re-adding a key is a REPLACEMENT, not a leak.
//   * capacity pressure EVICTS rather than failing, because nothing in the
//     streaming path frees from this pool yet (see the header's lifetime note).
//     A pool that failed instead would report allocFail against a P2 gate that
//     is specifically zero.
//
// A payload with no buffers is deliberate: nothing here flushes, so no render
// command is built, and FVoxelGpuBrickPayload's destructor returns early when
// every handle is null.
namespace
{
	FVoxelGpuBrickPayloadRef MakeTestBrickPayload(uint32 OccWords, uint32 MatWords)
	{
		FVoxelGpuBrickPayloadRef P = MakeShared<FVoxelGpuBrickPayload, ESPMode::ThreadSafe>();
		P->BrickCount = 64;
		P->OccWords = OccWords;
		P->MatWords = MatWords;
		return P;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickPoolBasicTest,
	"VoxelEarth.GpuPool.BrickBasic", kTestFlags)

bool FVoxelBrickPoolBasicTest::RunTest(const FString& Parameters)
{
	FVoxelBrickPool Pool;
	FVoxelBrickPoolConfig Config;
	Config.ChunkCapacity = 8;
	Config.OccWordCapacity = 8 * 64 * 16;
	Config.MatWordCapacity = 8 * 64 * 132;
	Pool.Init(Config);

	TSet<int32> Slots;
	for (int32 I = 0; I < 4; ++I)
	{
		const FVoxelBrickChunkKey Key{ I, 0, 0, 0 };
		const int32 Slot = Pool.AddChunkFromGpu(MakeTestBrickPayload(320, 900), Key, FVoxelBrickChunkShading::Neutral());
		TestNotEqual(TEXT("add succeeds"), Slot, int32(INDEX_NONE));
		TestFalse(TEXT("slots are distinct"), Slots.Contains(Slot));
		Slots.Add(Slot);

		FVoxelBrickPool::FResidentChunk R;
		TestTrue(TEXT("resident after add"), Pool.DebugGetResidentChunk(Key, R));
		// The whole reason ChunkSlot = BrickBase / 64 is legal.
		TestEqual(TEXT("descriptor block is 64-aligned"), R.BrickBase % 64u, 0u);
		TestEqual(TEXT("slot agrees with the descriptor base"), int32(R.BrickBase / 64u), Slot);
		TestEqual(TEXT("lookup by (level, chunk) finds it"), Pool.FindChunkSlot(Key), Slot);
	}

	TestEqual(TEXT("four chunks resident"), Pool.GetNumResidentChunks(), 4);
	TestEqual(TEXT("no allocation failures"), Pool.GetAllocFailures(), int64(0));
	TestEqual(TEXT("no evictions"), Pool.GetEvictions(), int64(0));
	TestEqual(TEXT("descriptor slots used"), Pool.GetUsedDescSlots(), 4u * 64u);
	TestEqual(TEXT("occupancy dwords used"), Pool.GetUsedOccWords(), 4u * 320u);
	TestEqual(TEXT("material dwords used"), Pool.GetUsedMatWords(), 4u * 900u);
	// 8 B per descriptor slot, 4 B per dword, 32 B per record -- the format's own
	// accounting, which is what a census is compared against.
	TestEqual(TEXT("resident bytes"), Pool.GetResidentBytes(),
	          uint64(4 * 64 * 8) + uint64(4 * 320 * 4) + uint64(4 * 900 * 4) + uint64(4 * 32));

	// A chunk that never existed is not resident, and asking is not an error.
	TestEqual(TEXT("absent key"), Pool.FindChunkSlot(FVoxelBrickChunkKey{ 99, 0, 0, 0 }),
	          int32(INDEX_NONE));
	// Level is PART of the key: same coordinate, different ring, different chunk.
	TestEqual(TEXT("level is part of the key"), Pool.FindChunkSlot(FVoxelBrickChunkKey{ 0, 0, 0, 1 }),
	          int32(INDEX_NONE));

	TestTrue(TEXT("remove succeeds"), Pool.RemoveChunk(FVoxelBrickChunkKey{ 0, 0, 0, 0 }));
	TestFalse(TEXT("double remove is refused, not fatal"),
	          Pool.RemoveChunk(FVoxelBrickChunkKey{ 0, 0, 0, 0 }));
	TestEqual(TEXT("three left"), Pool.GetNumResidentChunks(), 3);
	TestEqual(TEXT("its dwords came back"), Pool.GetUsedOccWords(), 3u * 320u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickPoolReplaceTest,
	"VoxelEarth.GpuPool.BrickReplace", kTestFlags)

bool FVoxelBrickPoolReplaceTest::RunTest(const FString& Parameters)
{
	FVoxelBrickPool Pool;
	FVoxelBrickPoolConfig Config;
	Config.ChunkCapacity = 8;
	Config.OccWordCapacity = 8 * 64 * 16;
	Config.MatWordCapacity = 8 * 64 * 132;
	Pool.Init(Config);

	const FVoxelBrickChunkKey Key{ 3, 4, 5, 2 };
	Pool.AddChunkFromGpu(MakeTestBrickPayload(160, 400), Key, FVoxelBrickChunkShading::Neutral());

	// A RE-MESH IS A REPLACEMENT. The old ranges must go back before the new ones
	// are asked for, or a chunk that churns ratchets the pool until it saturates
	// -- with the CPU-side table still reporting one chunk.
	for (int32 I = 0; I < 8; ++I)
	{
		Pool.AddChunkFromGpu(MakeTestBrickPayload(160, 400), Key, FVoxelBrickChunkShading::Neutral());
		TestEqual(TEXT("still exactly one chunk"), Pool.GetNumResidentChunks(), 1);
		TestEqual(TEXT("occupancy does not ratchet"), Pool.GetUsedOccWords(), 160u);
		TestEqual(TEXT("materials do not ratchet"), Pool.GetUsedMatWords(), 400u);
		TestEqual(TEXT("descriptors do not ratchet"), Pool.GetUsedDescSlots(), 64u);
	}
	TestEqual(TEXT("no evictions"), Pool.GetEvictions(), int64(0));
	TestEqual(TEXT("no allocation failures"), Pool.GetAllocFailures(), int64(0));

	// A re-add with DIFFERENT sizes must also be exact -- this is what a chunk
	// gaining geometry looks like.
	Pool.AddChunkFromGpu(MakeTestBrickPayload(1024, 8448), Key, FVoxelBrickChunkShading::Neutral());
	TestEqual(TEXT("grown chunk, exact occupancy"), Pool.GetUsedOccWords(), 1024u);
	TestEqual(TEXT("grown chunk, exact materials"), Pool.GetUsedMatWords(), 8448u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickPoolEvictionTest,
	"VoxelEarth.GpuPool.BrickEviction", kTestFlags)

bool FVoxelBrickPoolEvictionTest::RunTest(const FString& Parameters)
{
	// The pool announces its FIRST EVER eviction at Error severity, on purpose:
	// three invariants stop holding at that moment (parked-chunk adoption, the
	// never-exercised index Removed path, and the sizing headroom) and all three
	// fail SILENTLY. That is right for production and it makes every test that
	// deliberately exercises eviction fail, because the automation framework
	// promotes any logged Error into a test failure. Declare it rather than
	// lowering a production log level to suit a test -- the same call made for
	// "Brick pool REFUSED chunk" above.
	AddExpectedError(TEXT("BRICK POOL EVICTED FOR THE FIRST TIME"),
	                 EAutomationExpectedErrorFlags::Contains, 0);
	// Exactly four chunks' worth of everything, so the fifth add MUST displace
	// the first.
	FVoxelBrickPool Pool;
	FVoxelBrickPoolConfig Config;
	Config.ChunkCapacity = 4;
	Config.OccWordCapacity = 4 * 256;
	Config.MatWordCapacity = 4 * 1000;
	Pool.Init(Config);

	for (int32 I = 0; I < 10; ++I)
	{
		const int32 Slot = Pool.AddChunkFromGpu(MakeTestBrickPayload(256, 1000),
		                                        FVoxelBrickChunkKey{ I, 0, 0, 0 }, FVoxelBrickChunkShading::Neutral());
		TestNotEqual(TEXT("capacity pressure evicts rather than failing"), Slot, int32(INDEX_NONE));
	}

	// THE P2 GATE IS allocFail == 0, and this is the mechanism that keeps it
	// there while nothing in the streaming path frees from the pool.
	TestEqual(TEXT("no allocation failures"), Pool.GetAllocFailures(), int64(0));
	TestEqual(TEXT("six evicted"), Pool.GetEvictions(), int64(6));
	TestEqual(TEXT("four resident"), Pool.GetNumResidentChunks(), 4);
	// Oldest out, newest in.
	TestEqual(TEXT("the first chunk is gone"), Pool.FindChunkSlot(FVoxelBrickChunkKey{ 0, 0, 0, 0 }),
	          int32(INDEX_NONE));
	TestNotEqual(TEXT("the last chunk is resident"),
	             Pool.FindChunkSlot(FVoxelBrickChunkKey{ 9, 0, 0, 0 }), int32(INDEX_NONE));
	// And the arenas are exactly full rather than leaked into.
	TestEqual(TEXT("occupancy exactly full"), Pool.GetUsedOccWords(), 4u * 256u);
	TestEqual(TEXT("materials exactly full"), Pool.GetUsedMatWords(), 4u * 1000u);

	// A chunk larger than the whole arena cannot be made to fit by evicting
	// everything, and that is the one case that must REPORT rather than spin.
	//
	// The pool logs that refusal at Error severity, and the automation
	// framework promotes any Error logged during a test into a test failure --
	// so this test failed for doing precisely the thing it exists to prove.
	// Declare the expected error rather than lowering a production log level to
	// suit a test: in a live run "the brick pool refused a chunk" IS an error.
	AddExpectedError(TEXT("Brick pool REFUSED chunk"), EAutomationExpectedErrorFlags::Contains, 1);
	const int32 Refused = Pool.AddChunkFromGpu(MakeTestBrickPayload(4 * 256 + 1, 1000),
	                                           FVoxelBrickChunkKey{ 100, 0, 0, 0 }, FVoxelBrickChunkShading::Neutral());
	TestEqual(TEXT("an impossible chunk is refused"), Refused, int32(INDEX_NONE));
	TestEqual(TEXT("and counted"), Pool.GetAllocFailures(), int64(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickPoolFlushQueueTest,
	"VoxelEarth.GpuPool.BrickFlushQueue", kTestFlags)

bool FVoxelBrickPoolFlushQueueTest::RunTest(const FString& Parameters)
{
	// The pool announces its FIRST EVER eviction at Error severity, on purpose:
	// three invariants stop holding at that moment (parked-chunk adoption, the
	// never-exercised index Removed path, and the sizing headroom) and all three
	// fail SILENTLY. That is right for production and it makes every test that
	// deliberately exercises eviction fail, because the automation framework
	// promotes any logged Error into a test failure. Declare it rather than
	// lowering a production log level to suit a test -- the same call made for
	// "Brick pool REFUSED chunk" above.
	AddExpectedError(TEXT("BRICK POOL EVICTED FOR THE FIRST TIME"),
	                 EAutomationExpectedErrorFlags::Contains, 0);
	// WHY THIS TEST EXISTS, WRITTEN DOWN BECAUSE THE REASON IS A CRASH. Flush's
	// first ever run in an editor asserted inside FRDGBuilder::Execute() -- an
	// RDG event scope was still open, which nothing offline could have caught.
	// The dispatch half still cannot be reached from automation (it needs a real
	// RHI), but the two things around it can be, and they are the two a wrong
	// fix would quietly break: the EARLY-OUT that decides whether a render
	// command is enqueued at all, and the DROP RULE that makes recording clears
	// before writes sound.
	FVoxelBrickPool Pool;
	FVoxelBrickPoolConfig Config;
	Config.ChunkCapacity = 4;
	Config.OccWordCapacity = 4 * 256;
	Config.MatWordCapacity = 4 * 1000;
	Pool.Init(Config);

	// (1) THE EARLY-OUT. Nothing pending means no render command, which is the
	// only branch of Flush that is safe to execute without an RHI -- and the one
	// every idle tick takes, since Tick calls Flush unconditionally.
	TestEqual(TEXT("nothing pending on a fresh pool"), Pool.DebugGetPendingWriteCount(), 0);
	TestEqual(TEXT("no clears pending either"), Pool.DebugGetPendingClearCount(), 0);
	Pool.Flush();
	Pool.Flush();
	TestEqual(TEXT("an empty flush stays empty"), Pool.DebugGetPendingWriteCount(), 0);
	TestEqual(TEXT("and queues no clears"), Pool.DebugGetPendingClearCount(), 0);

	// (2) THE DROP RULE. One write queued per add.
	for (int32 I = 0; I < 3; ++I)
	{
		Pool.AddChunkFromGpu(MakeTestBrickPayload(256, 1000), FVoxelBrickChunkKey{ I, 0, 0, 0 }, FVoxelBrickChunkShading::Neutral());
	}
	TestEqual(TEXT("three writes queued"), Pool.DebugGetPendingWriteCount(), 3);
	TestEqual(TEXT("no clears yet"), Pool.DebugGetPendingClearCount(), 0);

	// Retiring a chunk whose write has not been dispatched must DROP that write,
	// not merely queue a clear after it. Both land in one graph, and clears are
	// recorded first -- so a surviving write would repopulate a slot whose arena
	// ranges have already been handed back.
	TestTrue(TEXT("remove succeeds"), Pool.RemoveChunk(FVoxelBrickChunkKey{ 1, 0, 0, 0 }));
	TestEqual(TEXT("its queued write was dropped"), Pool.DebugGetPendingWriteCount(), 2);
	TestEqual(TEXT("and a clear was queued for the slot"), Pool.DebugGetPendingClearCount(), 1);
	TestEqual(TEXT("the drop is counted, not silent"), Pool.GetWritesDropped(), int64(1));

	// A re-add of a still-pending key is the same rule from the other side: the
	// superseded write goes, exactly one write for the slot remains, and the
	// pool does not accumulate two writers for one range.
	Pool.AddChunkFromGpu(MakeTestBrickPayload(128, 500), FVoxelBrickChunkKey{ 0, 0, 0, 0 }, FVoxelBrickChunkShading::Neutral());
	TestEqual(TEXT("re-add replaces its own pending write"), Pool.DebugGetPendingWriteCount(), 2);
	TestEqual(TEXT("and counts the superseded one"), Pool.GetWritesDropped(), int64(2));

	// Eviction drops the evicted chunk's pending write for the same reason.
	//
	// COUNT THE RESIDENTS, DO NOT ASSUME THEM. Capacity is four chunks and TWO
	// are resident here, not three: chunk 1 was removed, and the re-add above
	// was a REPLACEMENT of chunk 0 (AddChunkFromGpu frees and re-adds a key it
	// already holds), not an addition. So 10 and 11 fill capacity exactly at
	// four and evict nothing -- this assertion sat looking green while testing
	// nothing at all. It takes three more to force one out.
	Pool.AddChunkFromGpu(MakeTestBrickPayload(256, 1000), FVoxelBrickChunkKey{ 10, 0, 0, 0 }, FVoxelBrickChunkShading::Neutral());
	Pool.AddChunkFromGpu(MakeTestBrickPayload(256, 1000), FVoxelBrickChunkKey{ 11, 0, 0, 0 }, FVoxelBrickChunkShading::Neutral());
	Pool.AddChunkFromGpu(MakeTestBrickPayload(256, 1000), FVoxelBrickChunkKey{ 12, 0, 0, 0 }, FVoxelBrickChunkShading::Neutral());
	TestTrue(TEXT("something was evicted"), Pool.GetEvictions() > 0);
	TestEqual(TEXT("no allocation failures"), Pool.GetAllocFailures(), int64(0));
	TestEqual(TEXT("one pending write per resident chunk, never more"),
	          Pool.DebugGetPendingWriteCount(), Pool.GetNumResidentChunks());

	// Deliberately NOT flushed: the dispatch half needs a real RHI, and a test
	// that enqueued it would be testing the render thread, not this.
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickPoolCpuArmTest,
	"VoxelEarth.GpuPool.BrickCpuArm", kTestFlags)

bool FVoxelBrickPoolCpuArmTest::RunTest(const FString& Parameters)
{
	// The pool announces its FIRST EVER eviction at Error severity, on purpose:
	// three invariants stop holding at that moment (parked-chunk adoption, the
	// never-exercised index Removed path, and the sizing headroom) and all three
	// fail SILENTLY. That is right for production and it makes every test that
	// deliberately exercises eviction fail, because the automation framework
	// promotes any logged Error into a test failure. Declare it rather than
	// lowering a production log level to suit a test -- the same call made for
	// "Brick pool REFUSED chunk" above.
	AddExpectedError(TEXT("BRICK POOL EVICTED FOR THE FIRST TIME"),
	                 EAutomationExpectedErrorFlags::Contains, 0);
	// WHAT THIS COVERS, AND WHAT IT CANNOT. The CPU arm exists because the brick
	// pool was fed only by the GPU mesh fork, which carried 5.2% of streaming
	// traffic. Its BYTES are not this test's business -- they come from
	// vxc::packChunkBricksCanonical, which has 15 test cases and 726 assertions
	// behind it and which voxel.GPU.VerifyBrickPack byte-compared the GPU kernel
	// against. What is new and therefore testable offline is the BOOKKEEPING:
	// that a CPU add takes the same allocations as a GPU add, that the two
	// producers cannot both hold one key, and that a malformed pack is refused
	// rather than half-written. The upload itself needs a real RHI and is not
	// reachable from here, exactly as the GPU arm's dispatch is not.
	auto MakeCpuPack = [](uint32 OccWords, uint32 MatWords)
	{
		FVoxelBrickCpuPackRef P = MakeShared<FVoxelBrickCpuPack, ESPMode::ThreadSafe>();
		P->Desc.SetNumZeroed(64 * 2);
		P->Occ.SetNumZeroed(int32(OccWords));
		P->Mat.SetNumZeroed(int32(MatWords));
		P->bAnySolid = true;
		return P;
	};

	FVoxelBrickPool Pool;
	FVoxelBrickPoolConfig Config;
	Config.ChunkCapacity = 8;
	Config.OccWordCapacity = 8 * 64 * 16;
	Config.MatWordCapacity = 8 * 64 * 132;
	Pool.Init(Config);

	const FVoxelBrickChunkKey KeyA{ 1, 2, 3, 0 };
	const int32 SlotA = Pool.AddChunkFromCpu(MakeCpuPack(320, 900), KeyA, FVoxelBrickChunkShading::Neutral());
	TestNotEqual(TEXT("a CPU add succeeds"), SlotA, int32(INDEX_NONE));
	TestEqual(TEXT("and is attributed to the CPU arm"), Pool.GetChunksAddedFromCpu(), int64(1));
	TestEqual(TEXT("which leaves the GPU arm at zero"), Pool.GetChunksAddedFromGpu(), int64(0));

	FVoxelBrickPool::FResidentChunk R;
	TestTrue(TEXT("resident after a CPU add"), Pool.DebugGetResidentChunk(KeyA, R));
	// The same invariant the GPU arm is held to, and for the same reason:
	// ChunkSlot = BrickBase / 64 is only sound while every descriptor allocation
	// is exactly 64 slots.
	TestEqual(TEXT("descriptor block is 64-aligned"), R.BrickBase % 64u, 0u);
	TestEqual(TEXT("occupancy words match the pack"), R.OccWords, 320u);
	TestEqual(TEXT("material words match the pack"), R.MatWords, 900u);
	TestEqual(TEXT("one write queued"), Pool.DebugGetPendingWriteCount(), 1);

	// THE TWO-PRODUCER RULE. An edit re-mesh reaches this pool with the key a
	// GPU job already packed. The re-add must FREE first and reuse the slot, or a
	// pool with two producers leaks one allocation per edit -- which is a leak
	// nothing in a screenshot could ever show.
	const uint32 UsedDescBefore = Pool.GetUsedDescSlots();
	const int32 SlotAgain = Pool.AddChunkFromGpu(MakeTestBrickPayload(320, 900), KeyA, FVoxelBrickChunkShading::Neutral());
	TestNotEqual(TEXT("the GPU arm can replace a CPU-added key"), SlotAgain, int32(INDEX_NONE));
	TestEqual(TEXT("still one chunk resident, not two"), Pool.GetNumResidentChunks(), 1);
	TestEqual(TEXT("and it took no extra descriptor slots"), Pool.GetUsedDescSlots(), UsedDescBefore);
	TestEqual(TEXT("the superseded write was dropped"), Pool.DebugGetPendingWriteCount(), 1);

	// A short descriptor array would leave the tail of a 64-slot allocation
	// holding whatever the previous tenant left, which reads as real terrain.
	// Refused, counted, and loud.
	AddExpectedError(TEXT("CPU brick add for chunk"), EAutomationExpectedErrorFlags::Contains, 1);
	FVoxelBrickCpuPackRef Short = MakeCpuPack(16, 16);
	Short->Desc.SetNum(100);
	const int64 FailBefore = Pool.GetAllocFailures();
	TestEqual(TEXT("a malformed pack is refused"),
	          Pool.AddChunkFromCpu(Short, FVoxelBrickChunkKey{ 9, 9, 9, 0 }, FVoxelBrickChunkShading::Neutral()), int32(INDEX_NONE));
	TestEqual(TEXT("and counted"), Pool.GetAllocFailures(), FailBefore + 1);

	// EVICTION ORDER. This is the half that only becomes visible once coverage
	// lands: FIFO evicts the chunks that loaded EARLIEST, which are the ones
	// nearest the player. With a focus set, the farthest chunk must go first --
	// so a near chunk added FIRST must survive a far chunk added LATER.
	FVoxelBrickPool Small;
	FVoxelBrickPoolConfig SmallConfig;
	SmallConfig.ChunkCapacity = 2;
	SmallConfig.OccWordCapacity = 2 * 64;
	SmallConfig.MatWordCapacity = 2 * 64;
	Small.Init(SmallConfig);
	// Focus at the origin, in level-0 voxels.
	Small.SetEvictionFocusVoxel0(0, 0, 0);
	const FVoxelBrickChunkKey Near{ 0, 0, 0, 0 };
	const FVoxelBrickChunkKey Far{ 500, 0, 0, 0 };
	Small.AddChunkFromCpu(MakeCpuPack(32, 32), Near, FVoxelBrickChunkShading::Neutral());   // added FIRST, and nearest
	Small.AddChunkFromCpu(MakeCpuPack(32, 32), Far, FVoxelBrickChunkShading::Neutral());    // added SECOND, and farthest
	Small.AddChunkFromCpu(MakeCpuPack(32, 32), FVoxelBrickChunkKey{ 1, 0, 0, 0 }, FVoxelBrickChunkShading::Neutral()); // forces one out
	TestEqual(TEXT("exactly one chunk was evicted"), Small.GetEvictions(), int64(1));
	TestEqual(TEXT("and it was ranked by distance, not insertion"),
	          Small.GetEvictionsByDistance(), int64(1));
	TestEqual(TEXT("the FAR chunk went"), Small.FindChunkSlot(Far), int32(INDEX_NONE));
	TestNotEqual(TEXT("and the NEAR one, added first, stayed"),
	             Small.FindChunkSlot(Near), int32(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickPoolChunkRecordTest,
	"VoxelEarth.GpuPool.BrickChunkRecord", kTestFlags)

bool FVoxelBrickPoolChunkRecordTest::RunTest(const FString& Parameters)
{
	// THE FIELD ORDER THE MARCHER IS ABOUT TO MIRROR, pinned against the WRITER
	// rather than against the doc.
	//
	// Why that distinction is not pedantry: the record is the one part of the
	// brick format a consumer cannot recover from anywhere else. A wrong
	// descriptor offset reads someone elses payload and usually looks broken; a
	// wrong LevelAndFlags bit reads as a chunk at the wrong RING SCALE, which is
	// a plausible-looking world. voxel.GPU.VerifyBrickPack already byte-compares
	// the GPU kernel against the CPU reference, but it needs an editor, an RHI
	// and a leg. This runs offline, so the layout is checked on every automation
	// pass rather than every gate run.
	//
	// Values are chosen so no two fields could be swapped without the test
	// noticing: distinct, non-zero, and asymmetric across the mask halves.
	uint32 Record[FVoxelBrickPool::kChunkRecordDwords] = {};
	const FIntVector Origin(-64, 96, 32);
	const uint64 Mask = 0x0123456789abcdefull;
	FVoxelBrickPool::BuildChunkRecord(Origin, /*RingLevel*/ 3, /*bAnySolid*/ true,
	                                  /*bAllSolid*/ false, /*BrickBase*/ 4096, Mask,
	                                  FVoxelBrickChunkShading::Neutral(), Record);

	TestEqual(TEXT("[0] is origin x, as an int32 bit pattern"), Record[0], uint32(int32(-64)));
	TestEqual(TEXT("[1] is origin y"), Record[1], uint32(96));
	TestEqual(TEXT("[2] is origin z"), Record[2], uint32(32));
	// level 3, anySolid set, allSolid clear.
	TestEqual(TEXT("[3] packs ring level in [0:3] and the two flags at [4] and [5]"),
	          Record[3], uint32(3u | (1u << 4)));
	TestEqual(TEXT("[4] is BrickBase"), Record[4], uint32(4096));
	TestEqual(TEXT("[5] is the LOW half of the L1 mask"), Record[5], uint32(0x89abcdefu));
	TestEqual(TEXT("[6] is the HIGH half"), Record[6], uint32(0x01234567u));
	TestEqual(TEXT("[7] is written, and written zero"), Record[7], uint32(0));

	// allSolid is the bit a producer that DERIVED it from the descriptors would
	// get wrong -- a brick can be fully solid and still MIXED -- so its position
	// is checked on its own rather than only in combination.
	uint32 Both[FVoxelBrickPool::kChunkRecordDwords] = {};
	FVoxelBrickPool::BuildChunkRecord(Origin, 5, true, true, 0, 0,
	                                  FVoxelBrickChunkShading::Neutral(), Both);
	TestEqual(TEXT("allSolid is bit 5, not bit 4 and not a second anySolid"),
	          Both[3], uint32(5u | (1u << 4) | (1u << 5)));

	uint32 Neither[FVoxelBrickPool::kChunkRecordDwords] = {};
	FVoxelBrickPool::BuildChunkRecord(Origin, 0, false, false, 0, 0,
	                                  FVoxelBrickChunkShading::Neutral(), Neither);
	TestEqual(TEXT("an all-air chunk records level 0 and no flags"), Neither[3], uint32(0));

	// A level that does not fit four bits MASKS, exactly as BrickChunkRecordMain
	// masks it. Pinned because the alternative -- clamping -- would silently
	// disagree with the GPU for the same input, and a byte-equality gate that
	// only ever sees levels 0..5 would never catch it.
	uint32 Wide[FVoxelBrickPool::kChunkRecordDwords] = {};
	FVoxelBrickPool::BuildChunkRecord(Origin, 0x13, false, false, 0, 0,
	                                  FVoxelBrickChunkShading::Neutral(), Wide);
	TestEqual(TEXT("the ring level MASKS to four bits rather than clamping"),
	          Wide[3], uint32(0x3));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
