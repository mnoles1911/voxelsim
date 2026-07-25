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

#endif // WITH_DEV_AUTOMATION_TESTS
