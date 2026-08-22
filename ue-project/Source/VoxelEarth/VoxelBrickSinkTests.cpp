// Tests for FDenseChunkSink -- the mesher handing its own voxel reads to the
// brick packer instead of the packer sampling them a second time
// (VoxelChunkMesher.h, VoxelWorldSubsystem.cpp namespace VoxelBrickCpuArm).
//
// WHY THIS FILE EXISTS, IN ONE SENTENCE: the optimisation is invisible when it
// is wrong. If the sink ever misses a cell, records an apron value, or transposes
// an axis, nothing errors and nothing looks different -- the mesh is untouched,
// the pool fills, every counter moves, and the resident brick volume quietly
// describes different terrain from the one on screen. That is the exact failure
// shape this project keeps paying for (a join computed instead of checked), and
// the only instrument that catches it before a marcher exists is this one.
//
// So the property under test is the strongest available and is checked cell by
// cell: after MeshChunkBricks runs with a sink attached, the dense array must
// equal the sampler's own answer for ALL 32,768 interior voxels of the chunk --
// not a checksum, not a count, the values.
//
// Run headlessly:
//   UnrealEditor-Cmd.exe VoxelEarth.uproject -unattended -nullrhi -nop4 \
//     -ExecCmds="Automation RunTests VoxelEarth.BrickSink; Quit"

#include "VoxelChunkMesher.h"
#include "VoxelCoords.h"
#include "VoxelFootprintBand.h" // the pre-dispatch skip policy under test
#include "VoxelMeshTypes.h" // FVoxelChunkQuad -- named directly below, so declared here
#include "Misc/AutomationTest.h"

#include "voxelcore/core.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags kBrickSinkTestFlags = EAutomationTestFlags::EditorContext
	                                                   | EAutomationTestFlags::ClientContext
	                                                   | EAutomationTestFlags::EngineFilter;

	constexpr int32 kSinkChunkEdge = VoxelCoords::ChunkEdgeVoxels; // 32
	constexpr int32 kSinkChunkCells = kSinkChunkEdge * kSinkChunkEdge * kSinkChunkEdge;

	// A duplicate of VoxelBrickCpuArm::FDenseChunkSink, and duplicated ON PURPOSE
	// rather than exported: that one lives in a .cpp because nothing outside the
	// streaming path should be able to attach a sink by accident. What is under
	// test is MeshChunkBricks' CONTRACT -- "every interior cell reaches the sink,
	// exactly once, with the material the sampler returned" -- and a sink written
	// here tests that contract rather than one particular implementation of it.
	struct FTestSink
	{
		static constexpr bool kRecords = true;
		vxc::MaterialId* Dense = nullptr;
		int32* Writes = nullptr;
		void Set(int32 Cx, int32 Cy, int32 Cz, vxc::MaterialId M) const
		{
			const int32 I = Cx + kSinkChunkEdge * (Cy + kSinkChunkEdge * Cz);
			Dense[I] = M;
			++Writes[I];
		}
	};

	// Terrain with something of everything the packer cares about: air, a solid
	// floor, a multi-material band (so bricks are MIXED rather than uniform), and
	// a scattered overhang so some bricks are fully solid and some are empty.
	// Deterministic and cheap -- this is a sampler, not a generator.
	vxc::MaterialId TestMaterialAt(int64 X, int64 Y, int64 Z)
	{
		const int64 Surface = 12 + ((X * 7 + Y * 13) & 7);
		if (Z > Surface)
		{
			// One floating slab, so at least one brick above the surface is solid
			// and at least one is pure air.
			return (Z == Surface + 5 && ((X + Y) & 3) == 0) ? vxc::MAT_ROCK : vxc::MAT_AIR;
		}
		if (Z == Surface) { return vxc::MAT_GRASS; }
		if (Z > Surface - 3) { return vxc::MAT_TOPSOIL; }
		return vxc::MAT_ROCK;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickSinkCoverageTest,
	"VoxelEarth.BrickSink.Coverage", kBrickSinkTestFlags)

bool FVoxelBrickSinkCoverageTest::RunTest(const FString& Parameters)
{
	const VoxelCoords::FVoxelChunkKey Key{ 3, -2, 0 };
	const auto Sampler = [](int64 X, int64 Y, int64 Z) { return TestMaterialAt(X, Y, Z); };

	TArray<vxc::MaterialId> Dense;
	Dense.SetNumZeroed(kSinkChunkCells);
	TArray<int32> Writes;
	Writes.SetNumZeroed(kSinkChunkCells);

	TArray<FVoxelChunkQuad> Quads;
	MeshChunkBricks(Key, Sampler, Quads, /*PerfCounters*/ nullptr, /*RingSkirtMask*/ 0,
	                FNeverSkipBrick(), FTestSink{ Dense.GetData(), Writes.GetData() });

	// (1) EVERY interior cell was written, and written exactly once. Once matters
	// as much as at-all: a cell written twice means two bricks think they own it,
	// which is an addressing bug that a value check alone can miss when both
	// writers happen to agree.
	int32 Unwritten = 0, Overwritten = 0;
	for (int32 I = 0; I < kSinkChunkCells; ++I)
	{
		if (Writes[I] == 0) { ++Unwritten; }
		else if (Writes[I] > 1) { ++Overwritten; }
	}
	TestEqual(TEXT("every interior voxel reached the sink"), Unwritten, 0);
	TestEqual(TEXT("and none of them reached it twice"), Overwritten, 0);

	// (2) The VALUES agree with the sampler, cell by cell. This is what makes the
	// brick volume and the mesh two encodings of one set of answers rather than
	// two opinions that happen to be close.
	int32 Mismatches = 0;
	int32 FirstBadIndex = INDEX_NONE;
	for (int32 Z = 0; Z < kSinkChunkEdge; ++Z)
	{
		for (int32 Y = 0; Y < kSinkChunkEdge; ++Y)
		{
			for (int32 X = 0; X < kSinkChunkEdge; ++X)
			{
				const int32 I = X + kSinkChunkEdge * (Y + kSinkChunkEdge * Z);
				const vxc::MaterialId Want = TestMaterialAt(int64(Key.X) * kSinkChunkEdge + X,
				                                            int64(Key.Y) * kSinkChunkEdge + Y,
				                                            int64(Key.Z) * kSinkChunkEdge + Z);
				if (Dense[I] != Want)
				{
					++Mismatches;
					if (FirstBadIndex == INDEX_NONE) { FirstBadIndex = I; }
				}
			}
		}
	}
	TestEqual(TEXT("the sink's voxels ARE the sampler's voxels"), Mismatches, 0);
	if (Mismatches > 0)
	{
		AddError(FString::Printf(TEXT("first mismatch at dense index %d"), FirstBadIndex));
	}

	// (3) The mesh is unaffected by the sink. The whole claim of the template
	// parameter is that attaching one changes nothing about what is drawn; a sink
	// that perturbed the quad stream would be a rendering change smuggled in as
	// an optimisation.
	//
	// Compared as BYTES rather than field by field: FVoxelChunkQuad is nine
	// uint8s and its layout is already a contract with VoxelQuadDecode.ush, so a
	// memcmp is both the strongest check available and the one that would notice
	// a field nobody thought to compare.
	TArray<FVoxelChunkQuad> RefQuads;
	MeshChunkBricks(Key, Sampler, RefQuads);
	TestEqual(TEXT("meshing with a sink emits the same number of quads"),
	          Quads.Num(), RefQuads.Num());
	if (Quads.Num() == RefQuads.Num() && Quads.Num() > 0)
	{
		TestEqual(TEXT("...and byte-identical ones"),
		          FMemory::Memcmp(Quads.GetData(), RefQuads.GetData(),
		                          SIZE_T(Quads.Num()) * sizeof(FVoxelChunkQuad)), 0);
	}
	// A chunk that meshed to nothing would make the comparison above vacuous.
	TestTrue(TEXT("the fixture chunk actually has geometry"), Quads.Num() > 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickSinkSkippedBricksTest,
	"VoxelEarth.BrickSink.SkippedBricks", kBrickSinkTestFlags)

bool FVoxelBrickSinkSkippedBricksTest::RunTest(const FString& Parameters)
{
	// THE CASE THE OPTIMISATION IS MOST LIKELY TO GET WRONG, and the one with no
	// visible symptom. The level-0 worker skips bricks it can prove emit no quad,
	// so meshBrick never runs for them and never reads their cells -- but a
	// marcher still has to know what is inside a buried brick. If the skip path
	// forgets to fill the sink, those bricks pack as AIR and the resident volume
	// has holes exactly where the terrain is most solid.
	//
	// Skipping EVERY brick is the strongest form of the test: nothing is meshed
	// at all, so the dense array can only be correct if the skip path fills it
	// entirely on its own.
	struct FAlwaysSkip
	{
		bool operator()(int32, int32, int32) const { return true; }
	};

	const VoxelCoords::FVoxelChunkKey Key{ -5, 6, 1 };
	const auto Sampler = [](int64 X, int64 Y, int64 Z) { return TestMaterialAt(X, Y, Z); };

	TArray<vxc::MaterialId> Dense;
	Dense.SetNumZeroed(kSinkChunkCells);
	TArray<int32> Writes;
	Writes.SetNumZeroed(kSinkChunkCells);

	TArray<FVoxelChunkQuad> Quads;
	MeshChunkBricks(Key, Sampler, Quads, /*PerfCounters*/ nullptr, /*RingSkirtMask*/ 0,
	                FAlwaysSkip(), FTestSink{ Dense.GetData(), Writes.GetData() });

	TestEqual(TEXT("skipping every brick emits no quads"), Quads.Num(), 0);

	int32 Unwritten = 0, Mismatches = 0;
	for (int32 Z = 0; Z < kSinkChunkEdge; ++Z)
	{
		for (int32 Y = 0; Y < kSinkChunkEdge; ++Y)
		{
			for (int32 X = 0; X < kSinkChunkEdge; ++X)
			{
				const int32 I = X + kSinkChunkEdge * (Y + kSinkChunkEdge * Z);
				if (Writes[I] != 1) { ++Unwritten; }
				const vxc::MaterialId Want = TestMaterialAt(int64(Key.X) * kSinkChunkEdge + X,
				                                            int64(Key.Y) * kSinkChunkEdge + Y,
				                                            int64(Key.Z) * kSinkChunkEdge + Z);
				if (Dense[I] != Want) { ++Mismatches; }
			}
		}
	}
	TestEqual(TEXT("a skipped brick still delivers its 512 cells, exactly once each"), Unwritten, 0);
	TestEqual(TEXT("and they are the right cells"), Mismatches, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickSinkSkirtTest,
	"VoxelEarth.BrickSink.Skirt", kBrickSinkTestFlags)

bool FVoxelBrickSinkSkirtTest::RunTest(const FString& Parameters)
{
	// THE ONE PLACE THE MESHER'S SAMPLER DELIBERATELY LIES. At a ring boundary the
	// apron is forced to AIR so a seam wall is emitted; that lie belongs to the
	// mesh and must never reach the brick volume, which describes what is THERE
	// rather than what should be drawn. The lie can only fire outside [0, 32) on a
	// flagged axis, so the interior should be identical with and without a skirt
	// -- and this checks that rather than trusting the bounds argument.
	const VoxelCoords::FVoxelChunkKey Key{ 0, 0, 0 };
	const auto Sampler = [](int64 X, int64 Y, int64 Z) { return TestMaterialAt(X, Y, Z); };
	const uint8 AllFaces = RingSkirt_NegX | RingSkirt_PosX | RingSkirt_NegY | RingSkirt_PosY;

	TArray<vxc::MaterialId> Skirted, Plain;
	Skirted.SetNumZeroed(kSinkChunkCells);
	Plain.SetNumZeroed(kSinkChunkCells);
	TArray<int32> WritesA, WritesB;
	WritesA.SetNumZeroed(kSinkChunkCells);
	WritesB.SetNumZeroed(kSinkChunkCells);

	TArray<FVoxelChunkQuad> QuadsA, QuadsB;
	MeshChunkBricks(Key, Sampler, QuadsA, nullptr, AllFaces, FNeverSkipBrick(),
	                FTestSink{ Skirted.GetData(), WritesA.GetData() });
	MeshChunkBricks(Key, Sampler, QuadsB, nullptr, 0, FNeverSkipBrick(),
	                FTestSink{ Plain.GetData(), WritesB.GetData() });

	int32 Mismatches = 0;
	for (int32 I = 0; I < kSinkChunkCells; ++I)
	{
		if (Skirted[I] != Plain[I]) { ++Mismatches; }
	}
	TestEqual(TEXT("the ring skirt never reaches the packed voxels"), Mismatches, 0);
	// And the skirt still did its job to the MESH, or this test proved nothing:
	// identical interiors with identical quad streams would mean the skirt mask
	// was simply ignored.
	TestTrue(TEXT("...while still changing the mesh, which is what it is for"),
	         QuadsA.Num() != QuadsB.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickSinkFillOrderTest,
	"VoxelEarth.BrickSink.FillOrder", kBrickSinkTestFlags)

bool FVoxelBrickSinkFillOrderTest::RunTest(const FString& Parameters)
{
	// TWO PRODUCERS OF ONE ARRAY, WHICH IS THE RISK THIS WHOLE WAVE KEEPS
	// RUNNING INTO.
	//
	// The dense chunk is filled two ways. Normally the MESHER fills it, brick by
	// brick, through FDenseChunkSink. Under voxel.Brick.SuppressQuadMesh there is
	// no mesher, so the PACKER fills it itself in column order -- x and y outer,
	// z innermost -- because every sampler on that path is column-shaped.
	//
	// Two orders, one array, one index expression. If those two ever disagree
	// about the layout, the suppression arm packs transposed terrain and reports
	// a perfectly healthy per-chunk cost for it -- a Phase 5 number measured
	// against a world that is not the world. Nothing else would notice, so this
	// does: same sampler, both fills, byte comparison.
	const VoxelCoords::FVoxelChunkKey Key{ 2, 9, 0 };
	const VoxelCoords::FVoxelLevelChunkKey LevelKey{ 0, Key };
	const auto Sampler = [](int64 X, int64 Y, int64 Z) { return TestMaterialAt(X, Y, Z); };

	TArray<vxc::MaterialId> ByMesher;
	ByMesher.SetNumZeroed(kSinkChunkCells);
	TArray<int32> Writes;
	Writes.SetNumZeroed(kSinkChunkCells);
	TArray<FVoxelChunkQuad> Quads;
	MeshChunkBricks(Key, Sampler, Quads, nullptr, 0, FNeverSkipBrick(),
	                FTestSink{ ByMesher.GetData(), Writes.GetData() });

	// The packer fill, transcribed from PackChunkMaterialising. Transcribed and
	// not called because that function lives in VoxelWorldSubsystem.cpp on
	// purpose -- nothing outside the streaming path should be able to fill a
	// chunk by accident -- and what is under test is the LAYOUT CONTRACT the two
	// share, not one particular caller of it.
	TArray<vxc::MaterialId> ByPacker;
	ByPacker.SetNumZeroed(kSinkChunkCells);
	for (int32 X = 0; X < kSinkChunkEdge; ++X)
	{
		for (int32 Y = 0; Y < kSinkChunkEdge; ++Y)
		{
			const int64 WX = int64(LevelKey.Key.X) * kSinkChunkEdge + X;
			const int64 WY = int64(LevelKey.Key.Y) * kSinkChunkEdge + Y;
			for (int32 Z = 0; Z < kSinkChunkEdge; ++Z)
			{
				ByPacker[X + kSinkChunkEdge * (Y + kSinkChunkEdge * Z)] =
					Sampler(WX, WY, int64(LevelKey.Key.Z) * kSinkChunkEdge + Z);
			}
		}
	}

	TestEqual(TEXT("the two fills produce byte-identical dense chunks"),
	          FMemory::Memcmp(ByMesher.GetData(), ByPacker.GetData(),
	                          SIZE_T(kSinkChunkCells) * sizeof(vxc::MaterialId)), 0);
	// And the fixture is not uniform, or a transposition would compare equal by
	// accident and this test would be green for the wrong reason.
	bool bVaries = false;
	for (int32 I = 1; I < kSinkChunkCells && !bVaries; ++I)
	{
		bVaries = ByPacker[I] != ByPacker[0];
	}
	TestTrue(TEXT("the fixture chunk is not uniform, so a transposition would show"), bVaries);
	return true;
}


// ---------------------------------------------------------------------------
// The pre-dispatch skip policy: all-air and all-solid are NOT interchangeable
// ---------------------------------------------------------------------------
//
// WHY THIS TEST EXISTS. BandProvesChunkEmpty answers "does this chunk mesh to
// zero quads", and both all-air and all-solid do. Acting on that by dropping the
// chunk is right for a mesher and wrong for a marcher: an absent all-solid chunk
// has no record, so the lookup misses and reads as EMPTY -- solid rock you can
// see through, with no counter moving anywhere.
//
// It went unnoticed because brick coverage is a fraction of chunks that GET a
// mesh job, and a chunk dropped before dispatch is not in that denominator. The
// metric is structurally incapable of showing it. So the property is pinned here
// instead, where it is cheap and where it can fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBandSkipPolicyTest,
	"VoxelEarth.BrickSink.BandSkipPolicy", kBrickSinkTestFlags)

bool FVoxelBandSkipPolicyTest::RunTest(const FString& Parameters)
{
	using namespace VoxelStreaming;
	constexpr int32 ChunkVox = VoxelCoords::ChunkEdgeVoxels;

	// An ALL-AIR chunk: its interior floor is above every column top.
	FFootprintBand AirBand;
	AirBand.MaxSurfaceTopVoxel = int32(-4 * ChunkVox);
	AirBand.SolidBelowVoxel = int32(-8 * ChunkVox);
	// An ALL-SOLID chunk: chunk and apron sit below the lowest z holding air.
	FFootprintBand SolidBand;
	SolidBand.MaxSurfaceTopVoxel = int32(64 * ChunkVox);
	SolidBand.SolidBelowVoxel = int32(32 * ChunkVox);

	bool bAllAir = false;

	// (1) The underlying proof is INDIFFERENT to which case it is -- that is the
	// whole trap, so it is asserted rather than assumed.
	TestTrue(TEXT("the proof holds for an all-air chunk"),
	         BandProvesChunkEmpty(AirBand, 0, bAllAir));
	TestTrue(TEXT("...and it reports air"), bAllAir);
	TestTrue(TEXT("the proof holds for an all-solid chunk too"),
	         BandProvesChunkEmpty(SolidBand, 0, bAllAir));
	TestFalse(TEXT("...and it reports NOT air"), bAllAir);

	// (2) With nothing reading chunk contents, the policy is the old behaviour
	// exactly. This is what keeps a run with the brick volume off unchanged.
	bool bKept = false;
	TestTrue(TEXT("volume not fed: an all-air chunk may be dropped"),
	         BandSkipMayDropChunk(AirBand, 0, /*bVolumeNeedsSolidChunks*/ false, bAllAir, &bKept));
	TestFalse(TEXT("...and nothing was kept for the volume"), bKept);
	TestTrue(TEXT("volume not fed: an all-solid chunk may also be dropped"),
	         BandSkipMayDropChunk(SolidBand, 0, false, bAllAir, &bKept));
	TestFalse(TEXT("...and nothing was kept"), bKept);

	// (3) THE FIX. With the volume fed, air may still be dropped -- absent and
	// empty read alike -- but solid may NOT, and the override is reported so a
	// silent fix and no fix do not look identical in a log.
	TestTrue(TEXT("volume FED: an all-air chunk may still be dropped"),
	         BandSkipMayDropChunk(AirBand, 0, /*bVolumeNeedsSolidChunks*/ true, bAllAir, &bKept));
	TestFalse(TEXT("...and it is not counted as kept, because it was not"), bKept);
	TestFalse(TEXT("volume FED: an all-solid chunk may NOT be dropped"),
	          BandSkipMayDropChunk(SolidBand, 0, true, bAllAir, &bKept));
	TestTrue(TEXT("...and the override IS counted"), bKept);

	// (4) A chunk the proof does not cover is untouched by any of this, and must
	// never be reported as kept -- otherwise the counter that is supposed to
	// equal the overridden set would count ordinary chunks and read healthy.
	FFootprintBand SurfaceBand;
	SurfaceBand.MaxSurfaceTopVoxel = int32(ChunkVox / 2);
	SurfaceBand.SolidBelowVoxel = int32(-4 * ChunkVox);
	TestFalse(TEXT("a straddling chunk is not proven empty"),
	          BandProvesChunkEmpty(SurfaceBand, 0, bAllAir));
	TestFalse(TEXT("...so it may not be dropped"),
	          BandSkipMayDropChunk(SurfaceBand, 0, true, bAllAir, &bKept));
	TestFalse(TEXT("...and it is NOT counted as an override"), bKept);

	// (5) The admission-time twin, whose proof is all-solid by construction.
	TestTrue(TEXT("all-solid admission proof may drop while nothing reads contents"),
	         AllSolidProofMayDropChunk(/*bVolumeNeedsSolidChunks*/ false));
	TestFalse(TEXT("...and may NOT once the volume is fed"),
	          AllSolidProofMayDropChunk(true));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
