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

#endif // WITH_DEV_AUTOMATION_TESTS
