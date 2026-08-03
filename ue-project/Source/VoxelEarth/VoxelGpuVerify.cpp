// voxel.GPU.VerifyRegion — the G2a gate (ADR-0006).
//
// WHAT THIS PROVES, AND WHY IT IS WORTH A CONSOLE COMMAND.
//
// voxel-core/bench/vxc_gpu.exe already proves the GPU kernels are bit-exact
// with the CPU mesher. But it proves it about kernels compiled by standalone
// DXC and run on raw Vulkan. Unreal compiles the same source with its own
// shader pipeline, binds resources its own way, and runs them on D3D12. None
// of that is guaranteed to produce identical bytes, and "the terrain looks a
// bit wrong" is a terrible way to find out.
//
// So this command runs the SAME two fixture regions the bench runs, in the
// same order, accumulating the digest with the same rules — and prints a
// number that should be *identical* to the one the bench prints. If it is,
// Unreal's toolchain reproduces the proven path exactly. If it is not, the
// per-field mismatch dump says precisely where the two disagree.
//
// It is deliberately headless: no PIE, no flying, no visual judgement.
//
// Usage:
//   voxel.GPU.VerifyRegion            both bench fixture regions (digest gate)
//   voxel.GPU.VerifyRegion 0          origin region only
//   voxel.GPU.VerifyRegion 1          far-negative region only

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "VoxelGpuWorldGen.h"
#include "VoxelGpuRegionBuild.h"
#include "VoxelMeshTypes.h" // PackVoxelChunkQuad / UnpackVoxelChunkQuad, for the D4 round-trip check
// The SHIPPING footprint-band reduction, lifted out of VoxelWorldSubsystem.cpp
// so this gate compares BandReduceMain against it rather than a transcription.
#include "VoxelFootprintBand.h"

#include "voxelcore/core.h"
#include "voxelcore/tiles.h"
#include "voxelcore/amplifier.h"
#include "voxelcore/mesher.h"
#include "voxelcore/caverns.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGpuVerify, Log, All);

namespace
{
	// The bench's fixtures, verbatim (gpu_harness.cpp main()). Same seed, same
	// origins, same 64x64 footprint — this is what makes the digests
	// comparable across the two toolchains.
	constexpr uint64 kSeed = 20260719;

	// Digest of the CPU REFERENCE over both fixture regions — same fields, same
	// order as the GPU digest, but folded from vxc::Amplifier / vxc::meshBrick
	// instead of from the readback. Nothing about the GPU enters it.
	//
	// It exists to answer "which side moved?" before anyone reads a per-cell
	// mismatch. Re-pin it ONLY on a deliberate vxc::kWorldGenVersion bump, and
	// in the same commit as the worldgen.ush re-mirror and the SPIR-V respin.
	//
	// It coincides with the GPU digest today, and must: when the gate passes,
	// the two folds see identical bytes. The value is still worth pinning
	// separately, because GPU-vs-CPU equality cannot detect BOTH sides moving
	// together — which is the shape of every worldgen-version accident.
	//
	// THE INSTRUCTION ABOVE WAS NOT FOLLOWED FOR THIRTEEN VERSIONS. This was
	// pinned at kWorldGenVersion 8 and sat unchanged through v9..v20 while the
	// tree bumped past it, so the gate reported a mismatch on every run and the
	// mismatch meant nothing. A guard that cries wolf at every version is not a
	// weaker guard, it is an absent one -- and it is one of only two things
	// covering the STALE voxelcore.lib case that worldgen.ush's #error
	// deliberately does not (see its SCOPE note).
	//
	// So the version is pinned ALONGSIDE the digest and asserted, which is what
	// makes the discipline mechanical instead of remembered: bump
	// kWorldGenVersion without re-measuring this and the build stops.
	constexpr uint32 kExpectedCpuDigestWorldGenVersion = 25;
	static_assert(vxc::kWorldGenVersion == kExpectedCpuDigestWorldGenVersion,
	              "vxc::kWorldGenVersion moved without kExpectedCpuDigest being re-measured. "
	              "Run voxel.GPU.VerifyRegion over BOTH fixture regions, take the 'got' value "
	              "out of the CPU REFERENCE DIGEST MISMATCH line, and update both constants in "
	              "the same commit as the worldgen.ush re-mirror and the SPIR-V respin. Do NOT "
	              "just widen this assert -- the pin is the only check that catches the CPU and "
	              "the GPU moving together.");

	// Measured 2026-08-01 at kWorldGenVersion 21, over BOTH fixture regions,
	// against build/voxel-core-msvc/voxelcore.lib rebuilt from 4ff655b.
	//
	// The previous value (0x6e893ab3679a8c81, v8) was additionally a ONE-REGION
	// fold wearing a two-region contract: kRegions[0] "origin" is 2 bricks tall
	// and its dispatch was refused outright by the mesh chain, so it contributed
	// nothing. BuildRequest now drops bMeshChain on a region too thin to mesh,
	// exactly as the bench does, so worldgen still runs there and its columns and
	// cells fold in. That is why this value differs from a v21 measurement taken
	// before that fix (0x23cd9b86724c4e2b) -- same worldgen, one more region.
	constexpr uint64 kExpectedCpuDigest = 0xb2b5d2f1044caa35ull;

	// v22 RE-MEASURED IT AND IT DID NOT MOVE, WHICH IS A FINDING, NOT A PASS.
	// v22 changed classifyBiome's savanna gate from bio_4 to bio_15 (core.h's
	// changelog has the why). Re-measuring the fold gave the identical value,
	// and the reason is that NEITHER FIXTURE CAN REACH THAT GATE: over all 8,192
	// columns of the two regions the synthetic sampler's climate spans
	// temperature u8 [66, 114] against kBiomeTempWarmU8 = 185, and precipitation
	// u8 [47, 66] against kBiomePrecipModU8 = 34, so ZERO columns are both warm
	// and inside the savanna precipitation window. Old gate fires 0 times, new
	// gate fires 0 times.
	//
	// So this pin says nothing about whether v22 is right, and the CPU/GPU
	// parity gate does not cover the changed branch at all. That is a fixture
	// gap of exactly the shape the SyntheticTileSampler comment warns about
	// ("if a threshold is not crossed here it is never checked for CPU/GPU
	// agreement anywhere") -- the sampler's RANGES span every threshold, but
	// these two 6.4 m windows sample a narrow, cool, wet slice of them. A
	// savanna-bearing fixture has to be added to kBandOnlyRegions rather than to
	// kRegions, so this pin stays comparable to the bench.
	//
	// ONE HAS ALREADY BEEN FOUND, by the same search that produced the band-only
	// origins, and gpu_harness.cpp carries it as its "savanna-boundary" fixture:
	// voxel (-249632, 1151968), 64x64 at 30000 mm pitch, sitting on a tile-pixel
	// corner where the 2x2 climate block straddles the CV threshold. It holds
	// 2,133 SAVANNA columns against 1,963 TEMPERATE_FOREST, so the boundary
	// between them is set by the faded-bilinear blend of the byte v22 moved.
	//
	// v23 DID NOT MOVE IT EITHER, AND FOR THE SAME KIND OF REASON. v23 caps the
	// SUM of the two detail pools instead of each pool separately, and it is
	// gated on `fine` -- so it can only change the FINE tier. Both fixture
	// regions are COARSE tier (30000 mm pitch), so v23's branch is never taken
	// here and the fold is arithmetically bound to be unchanged. Re-measured
	// through UE's own RDG path anyway: b2b5d2f1044caa35, identical.
	//
	// So read this pin correctly. It confirms that v23 did not disturb the
	// coarse tier -- which IS worth confirming, because the coarse path was
	// supposed to be untouched and vxc_bench --radius 16 --digest independently
	// agrees (cca9b86e78da033e, byte-identical to main). It says NOTHING about
	// whether v23's fine-tier change is right; nothing in the CPU/GPU parity
	// gate exercises it. The evidence for v23 is the drainage measurement
	// (stranded area 35.5% -> 3.6% on the worst clean site) plus the
	// same-ground terracing A/B, not this constant.
	//
	// A FINE-TIER FIXTURE IS THE GAP TO CLOSE. Like the savanna one above it
	// belongs in kBandOnlyRegions, at a pitch <= kFineTierMaxPixelMm and on
	// ground steep enough that the summed detail gradient actually exceeds the
	// carrier -- otherwise the new cap is inert there too and the fixture would
	// be decorative.
	// vxc_gpu passes bit-exact on it; this path does not run it yet.
	//
	// Measured 2026-08-01 at kWorldGenVersion 22 by a standalone mirror of the
	// CPU-side fold below, validated by reproducing the v21 value byte for byte
	// against the same tree before the change was applied.
	//
	// v24 DID NOT MOVE IT EITHER, AND THIS TIME THE REASON IS THE SHARPEST YET
	// -- IT IS THE SAME GAP THE kBandOnlyRegions COMMENT BELOW DESCRIBES, SEEN
	// FROM THE OTHER SIDE. v24 (docs/underground-system-plan.md W2) waypoints
	// every cave edge into a two-segment polyline and interpolates its radius,
	// which moves every tunnel axis in the world. The fold is unchanged.
	//
	// The reason is NOT that the fixtures have no caves. They do: over the
	// "origin" fixture's 4,096 columns, 2,703 have cave segments at v23 and
	// 2,027 at v24 -- the change is enormous there. The reason is that the
	// fold's CELL range is derived from the topmost solid voxel of each column
	// and is 2 bricks tall, i.e. the SURFACE, while the roof guarantee keeps
	// every cave voxel at least 6 m below it. Carved cells inside the compared
	// range: 0 at v23, 0 at v24. Neither fixture contains a sinkhole shaft (the
	// one construct allowed through the roof), so nothing reaches the band.
	//
	// So the compared volume misses the cave pass entirely, and has for its
	// whole life -- exactly the failure mode this file already records twice
	// ("a gate that closes everywhere in the fixture is indistinguishable from
	// a term that is not mirrored at all"), one level down: a term that fires
	// OUTSIDE the compared volume is indistinguishable from one that is not
	// mirrored at all. voxel-core/bench/gpu_harness.cpp had the identical hole
	// and v24 closes it there with a "cave-band" fixture and a per-region
	// compareDepthVox that reaches the cave envelope; vxc_gpu now compares
	// 2.67M cells instead of 1.07M and its digest moved for the first time on a
	// cave change. THE EQUIVALENT FIXTURE IS STILL MISSING HERE, and it belongs
	// in kBandOnlyRegions (which does not feed this pin) so that closing the
	// hole does not turn this cross-toolchain gate into a re-baselining
	// exercise -- the same argument the savanna and fine-tier gaps above are
	// already parked under.
	//
	// HOW THE "unchanged" WAS ESTABLISHED, stated plainly because it was NOT
	// re-measured through UE's own RDG path: another agent held the box's one
	// editor slot, so voxel.GPU.VerifyRegion was not run. Instead the standalone
	// mirror above was extended to report, per fixture, every ColumnSample field
	// and every Amplifier::materialAt value over the exact compared cell range,
	// and run against BOTH trees -- v23 from main and v24 from this branch. The
	// columns+cells fold is byte-identical between them (0x0badfe3a5dec55d2 on
	// both). Since the mesher's entire input is those cells, the quads are
	// identical too, so the full fold cannot have moved. That is a DIFFERENTIAL
	// proof rather than an absolute re-measurement: the standalone mirror covers
	// columns and cells but not the raster-window construction, so it does not
	// reproduce this constant's absolute value and is not expected to. Anyone
	// with the editor should re-run voxel.GPU.VerifyRegion to confirm
	// absolutely; the prediction on record is that it prints b2b5d2f1044caa35.

	// v25 DID NOT MOVE IT EITHER, AND THIS TIME IT WAS ESTABLISHED BEFORE THE
	// FACT RATHER THAN GUESSED AT. v25 (docs/underground-system-plan.md W3)
	// replaces the sinkhole bore with an entrance CAVITY: a level floor at
	// absolute z, a lens roof clipped by the real ground, a rim warped by value
	// noise, a new field on CaveColumn and a new per-voxel compare. It moves
	// every entrance in the world, and the fold is unchanged.
	//
	// WHY, in one line: neither digest fixture contains an entrance. Not "no
	// entrance reaches the compared band" -- no entrance is in the 6.4 m window
	// at all. Entrance candidate nodes sit on a 102.4 m grid and one in four is
	// open, so a 6.4 m window contains one about once in 250 tries.
	//
	// HOW THAT WAS ESTABLISHED WITHOUT THE EDITOR (another agent holds the
	// box's one slot, again). Two independent statements, both from
	// voxel-core/bench/gpu_harness.cpp, whose "origin" and "far-negative"
	// fixtures are these two verbatim:
	//
	//   1. DIRECTLY. That harness now prints, per region, how many columns
	//      actually carry a cave / an entrance / a cavern -- added by W3 for
	//      exactly this reason, because the previous fixture comment ASSERTED
	//      an entrance was present and was wrong for a whole worldgen version.
	//      Both regions report 0 entrance columns of 4,096.
	//   2. DIFFERENTIALLY. Building that harness at v25 with its new entrance
	//      fixture removed reproduces the v24 digest 5d3fc4a8e8366148 byte for
	//      byte over all ten pre-existing regions. Since the fold below digests
	//      ColumnSample fields and Amplifier::materialAt over the compared
	//      cells, and those are byte-identical, this constant cannot have moved.
	//
	// The prediction on record is therefore that voxel.GPU.VerifyRegion still
	// prints b2b5d2f1044caa35 at v25. Anyone with the editor should confirm it
	// absolutely -- and note that the v24 prediction made the same way WAS
	// confirmed by a later run (Saved/gpuverify-v24d.log: "CPU reference digest:
	// b2b5d2f1044caa35 (matches the pinned value; vxc::kWorldGenVersion = 24)").
	//
	// THE FIXTURE GAP ITSELF IS STILL OPEN HERE and is now measured rather than
	// suspected: kBandOnlyRegions gained shaft- and cavern-bearing fixtures at
	// v24, but the PINNED regions still see no entrance and no cavern, so this
	// constant remains a statement about surface worldgen only. Closing it means
	// adding an entrance-bearing region to kBandOnlyRegions (which does not feed
	// this pin), not to kRegions -- the same argument the savanna and fine-tier
	// gaps above are parked under.

	struct FRegionSpec
	{
		const TCHAR* Name;
		int32 OriginVx;
		int32 OriginVy;
		uint32 Width;
		uint32 Height;
		// D5. 0 for every digest fixture, and that is load-bearing: the pinned
		// 6e893ab3679a8c81 is a LEVEL-0 statement about the bench, so a coarse
		// run must never fold into it. voxel.GPU.VerifyCoarse builds its own
		// specs and its own digests.
		int32 CoarseLevel = 0;
	};

	// Mirror of vxc::GeneratedWorld::coarseRep and of worldgen.ush's coarseRep.
	// Three copies now, which is two more than anyone wants -- but the CPU one
	// is constexpr inside voxel-core, the HLSL one cannot call it, and this one
	// is the reference the other two are checked against. They agree by being
	// the same two operations, not by being shared.
	constexpr int64 CoarseRep(int64 C, int32 Level)
	{
		const int64 S = int64(1) << Level;
		return C * S + S / 2;
	}

	const FRegionSpec kRegions[] = {
		{ TEXT("origin"),        -64,     -64, 64, 64 },
		{ TEXT("far-negative"), -100000, 250000, 64, 64 },
	};

	// Builds one region's GPU request: the raster window, its contents, and the
	// z-range, exactly as gpu_harness.cpp::runRegion does.
	//
	// The raster window sizing itself lives in VoxelGpuRegionBuild.h so the async
	// runner's harness cannot get it subtly different from this proven one.
	//
	// OutCpuColumns is filled as a side effect because deriving the z-range
	// requires every column anyway, and the comparison needs them again later.
	// BAND-ONLY FIXTURES. These do NOT contribute to the digest.
	//
	// WHY THEY HAD TO EXIST AT ALL, and it is a fixture bug rather than a kernel
	// one. The two digest fixtures above are 64 columns square -- 6.4 m of world.
	// The cave lattice is kCaveLatticeMm = 25.6 m, so a 6.4 m patch usually
	// contains NO lattice node, and neither of them contains one: the D6 sweep
	// reported "0 cave / 0 shaft / 0 cavern" on every probe, across all 4,096
	// columns of BOTH fixtures once BandOriginJ let it search off the diagonal.
	// The cave-segment loop, the shaft term, the two bedrock clamps and
	// ceilSqrtU were therefore passing vacuously -- and that is precisely the
	// code the INT64 sentinel defect lived in.
	//
	// 320 columns is 32 m, which spans more than one full lattice period on both
	// axes and straddles the node at world (0,0), so it contains cave sites by
	// construction rather than by luck. Deliberately anchored to include voxel 0:
	// the "origin" fixture runs -64..-1 and stops one voxel short of it.
	//
	// SEPARATE FROM kRegions BECAUSE THE DIGEST IS PINNED. 6e893ab3679a8c81 is
	// compared against the bench, and adding a region to the digest loop would
	// change it -- turning a loud cross-toolchain gate into a re-baselining
	// exercise. These run bMeshChain = false (the band is a pure function of the
	// columns) so they cost columns and one reduction, not a mesh chain.
	// Each is 320 columns (32 m) anchored so that it STRADDLES a lattice node of
	// the feature it is named for, rather than hoping one falls inside:
	//
	//   cave   nodes every kCaveLatticeMm            = 25.6 m  =   256 voxels
	//   shaft  nodes every 4 cave nodes              = 102.4 m =  1024 voxels
	//          (kCaveShaftNodeMask 3), and only 1 IN 4 of those opens
	//          (kCaveShaftGateMask 3) -- so a single node is a 25% chance and
	//          two candidates are carried
	//   cavern nodes every kCavernCoarseLatticeRatio = 204.8 m =  2048 voxels
	//
	// A fixture that reports zero of its feature is not a failure; the sweep
	// says so out loud and the probes below it are then vacuous for that path.
	// That is the whole reason these exist -- the original two fixtures were
	// 6.4 m across against a 25.6 m lattice and reported 0/0/0 for months.
	// EVERY ONE OF THESE ORIGINS WAS FOUND BY SEARCH, NOT CHOSEN BY EYE, and the
	// difference is the whole point. Five hand-aligned anchors covering 512,000
	// columns previously found zero shafts and zero caverns -- which is exactly
	// what the constants predict, since shaft nodes are 1024 voxels apart and
	// only 1 in 4 opens, so a 320-column window contains one about 2% of the
	// time. voxel.GPU.FindBandFixture walks vxc::ColumnSample directly and
	// reports coordinates; these came out of it at seed 20260719:
	//
	//   shaft  column at voxel (-896, -16336)
	//   cavern column at voxel (-4240, -16384)
	//
	// Each fixture is 320 columns (32 m) CENTRED on its find, so the feature is
	// interior rather than clipped by an edge.
	//
	// IF THE SEED CHANGES THESE GO STALE, and they go stale QUIETLY -- the sweep
	// keeps passing, it just reports 0 shaft / 0 cavern again. That warning line
	// is the tripwire; re-run FindBandFixture when it fires.
	// EXTRA FIXTURES FOR THE COARSE SWEEP ONLY -- never for the digest loop.
	//
	// D5.2 could not prove level 5. Both digest fixtures pass L0-L4 and then L5's
	// [far-negative] dispatch is REFUSED with "Mesh chain needs >= 3 bricks per
	// axis (have 8, 8, 2)". At level 5 a coarse cell is 32 level-0 voxels, so a
	// 64-cell region spans 204.8 m and that fixture's entire terrain range
	// collapses into 2 coarse bricks of z; the halo then leaves no interior brick
	// to mesh. The FIXTURE ran out of vertical relief -- the coarse path never
	// disagreed.
	//
	// Three coarse bricks of z needs 3 * 8 * 32 = 768 level-0 voxels of surface
	// spread inside that window. voxel.GPU.FindBandFixture now searches for the
	// most mountainous such window rather than leaving anyone to guess at
	// alpine-looking coordinates; at seed 20260719 it reports 2,025 voxels
	// (202 m) at voxel (5120, -40960), which is 2.6x what L5 needs.
	//
	// SEPARATE FROM kRegions BECAUSE 6e893ab3679a8c81 IS PINNED. Adding a third
	// region to the digest loop would change the value and turn a loud
	// cross-toolchain gate into a re-baselining exercise.
	const FRegionSpec kCoarseExtraRegions[] = {
		{ TEXT("high-relief"), 5120, -40960, 64, 64 },
	};

	const FRegionSpec kBandOnlyRegions[] = {
		{ TEXT("cave-bearing"),    -64,    -64, 320, 320 },
		{ TEXT("shaft-bearing"), -1056, -16496, 320, 320 },
		{ TEXT("cavern-bearing"),-4400, -16544, 320, 320 },
	};

	FVoxelGpuRegionRequest BuildRequest(const FRegionSpec& Region,
	                                    const vxc::Amplifier& CpuAmp,
	                                    vxc::SyntheticTileSampler& Tiles,
	                                    TArray<vxc::ColumnSample>& OutCpuColumns)
	{
		FVoxelGpuRegionRequest Req;
		Req.DispatchColumns = FUintVector2(Region.Width, Region.Height);
		Req.OriginVx = Region.OriginVx;
		Req.OriginVy = Region.OriginVy;
		Req.Seed = kSeed;
		// BEFORE FillRasterWindow, which now READS it: at level L the window a
		// dispatch touches is 2^L wider than its column count, so a window sized
		// with CoarseLevel still 0 is silently too narrow and the kernel clamps.
		// Setting it afterwards is a no-op that looks like a fix -- which is
		// exactly what it was for one run of the D5 gate.
		Req.CoarseLevel = Region.CoarseLevel;

		VoxelGpuRegionBuild::FillRasterWindow(Req, Tiles);

		// --- columns + vertical extent --------------------------------------
		// Mirrors GeneratedWorld<8>::surfaceBrickRange, but reduced over the
		// whole region rather than one 8x8 footprint, because BrickZMin and
		// BricksZ are single scalars shared by the entire dispatch.
		OutCpuColumns.SetNumUninitialized(Region.Width * Region.Height);
		int64 VzMin = MAX_int64;
		int64 VzMax = MIN_int64;
		for (uint32 Y = 0; Y < Region.Height; ++Y)
		{
			for (uint32 X = 0; X < Region.Width; ++X)
			{
				// D5: at level L the dispatch's OriginVx/tid are LEVEL-L cell
				// indices and the column is sampled at the cell's representative
				// level-0 coordinate -- exactly vxc::coarseColumns. Identity at 0.
				const int64 Vx = CoarseRep(int64(Region.OriginVx) + X, Region.CoarseLevel);
				const int64 Vy = CoarseRep(int64(Region.OriginVy) + Y, Region.CoarseLevel);
				const vxc::ColumnSample C = CpuAmp.column(Vx, Vy);
				OutCpuColumns[int32(X + Y * Region.Width)] = C;

				// Topmost solid voxel: its centre (vz*100 + 50) <= surfaceMm.
				const int64 Top0 = vxc::floorDiv(int64(C.surfaceMm) - vxc::kVoxelSizeMm / 2,
				                                 vxc::kVoxelSizeMm);
				// ...then to the LEVEL-L cell holding it, mirroring
				// vxc::coarseSurfaceBrickRange. Identity at level 0.
				const int64 S = int64(1) << Region.CoarseLevel;
				const int64 Top = vxc::floorDiv(Top0 - S / 2, S);
				VzMin = FMath::Min(VzMin, Top);
				VzMax = FMath::Max(VzMax, Top);
			}
		}

		const int32 BrickZMin = static_cast<int32>(vxc::floorDiv(VzMin, 8));
		const int32 BrickZMax = static_cast<int32>(vxc::floorDiv(VzMax, 8));
		Req.BrickZMin = BrickZMin;
		Req.BricksZ = static_cast<uint32>(BrickZMax - BrickZMin + 1);

		// A REGION TOO THIN TO MESH RUNS WORLDGEN-ONLY, exactly as the bench
		// does ("region too thin for interior mesh bricks (bricksZ=N) — mesh
		// pass skipped", gpu_harness.cpp). bMeshChain defaults TRUE and nothing
		// here used to lower it, so RunRegionBlocking refused the whole dispatch
		// with "Mesh chain needs >= 3 bricks per axis" and the region contributed
		// NOTHING -- no columns, no cells, no digest.
		//
		// WHY THAT MATTERED MORE THAN IT LOOKS. kRegions[0] "origin" is 2 bricks
		// tall on this terrain and always has been (verified 2026-08-01 by
		// building vxc_gpu at ecda4b6, before the v20 brick-range change:
		// identical "brick z [1457, 1458]"). So one of the TWO fixtures the
		// pinned CPU digest is documented to cover has been silently absent from
		// it, and kExpectedCpuDigest was a one-region fold wearing a two-region
		// contract. The coarse path at D5 already handled this case as a SKIP;
		// the level-0 digest path did not.
		//
		// Skipping only the MESH here, rather than the region, is what keeps the
		// digest honest: ColumnMain and VoxelizeMain still run, columns and cells
		// still fold, and only the quad fold is absent -- which is precisely what
		// the bench folds for the same fixture, so the two toolchains stay
		// comparable, which is the whole reason these fixtures are copied
		// verbatim.
		const uint32 BricksX = Req.DispatchColumns.X / 8u;
		const uint32 BricksY = Req.DispatchColumns.Y / 8u;
		if (BricksX < 3 || BricksY < 3 || Req.BricksZ < 3)
		{
			Req.bMeshChain = false;
		}

		return Req;
	}

	constexpr uint32 CellIndexInBrick(uint32 X, uint32 Y, uint32 Z)
	{
		return X + 8u * (Y + 8u * Z);
	}

	struct FMismatch
	{
		FString Where;
		int64 Cpu;
		int64 Gpu;
	};

	// Mismatch counts split by WHICH STAGE disagreed, uncapped.
	//
	// THIS SPLIT IS THE POINT, and it is here because its absence produced a
	// published wrong diagnosis. When the gate went red in July 2026 the
	// printed list was capped at 20 and was quoted into docs/backlog.md with
	// only its `cell(...)` lines; the `col(...).topsoilMm` lines above them
	// were dropped, and the entry then asserted "all 4,096 columns match" —
	// which pointed the whole investigation at the compiled kernel when the
	// actual fault was a stale voxelcore.lib whose CPU worldgen predated the
	// v8 climate landing. Column fields disagreeing is a completely different
	// failure from cells-only disagreeing, so print the classification rather
	// than leaving it to be inferred from a truncated list.
	struct FTally
	{
		int64 Columns = 0;
		int64 Cells = 0;
		int64 Quads = 0;

		int64 Total() const { return Columns + Cells + Quads; }
	};

	// Compares one region and folds its output into the running digest, in the
	// bench's exact order: per column, the five column fields then that
	// column's whole vertical cell stack; then, after every column, the quads.
	// The order is load-bearing — a digest computed in a different order is
	// not comparable to the bench's, even when every byte matches.
	bool CompareRegion(const FRegionSpec& Region,
	                   const FVoxelGpuRegionRequest& Req,
	                   const FVoxelGpuRegionResult& Gpu,
	                   const TArray<vxc::ColumnSample>& CpuColumns,
	                   vxc::Digest& Digest,
	                   vxc::Digest& CpuDigest,
	                   FTally& Tally,
	                   TArray<FMismatch>& OutMismatches)
	{
		constexpr int32 kMaxMismatchesPrinted = 20;
		const uint32 BricksX = Region.Width / 8;
		const uint32 BricksZ = Req.BricksZ;

		for (uint32 Y = 0; Y < Region.Height; ++Y)
		{
			for (uint32 X = 0; X < Region.Width; ++X)
			{
				const int32 ColIdx = int32(X + Y * Region.Width);
				const FVoxelGpuColumnSample& G = Gpu.Columns[ColIdx];
				const vxc::ColumnSample& C = CpuColumns[ColIdx];

				Digest.u32(static_cast<uint32_t>(G.SurfaceMm));
				Digest.u32(static_cast<uint32_t>(G.TopsoilMm));
				Digest.u32(static_cast<uint32_t>(G.SubsoilMm));
				Digest.u32(static_cast<uint32_t>(G.BedrockDepthMm));
				Digest.u8(static_cast<uint8_t>(G.SurfaceMat));

				// Same fields, same order, from the CPU side. See kExpectedCpuDigest.
				CpuDigest.u32(static_cast<uint32_t>(C.surfaceMm));
				CpuDigest.u32(static_cast<uint32_t>(C.topsoilMm));
				CpuDigest.u32(static_cast<uint32_t>(C.subsoilMm));
				CpuDigest.u32(static_cast<uint32_t>(C.bedrockDepthMm));
				CpuDigest.u8(static_cast<uint8_t>(C.surfaceMat));

				const auto Record = [&](const TCHAR* Field, int64 CpuVal, int64 GpuVal)
				{
					if (CpuVal != GpuVal)
					{
						++Tally.Columns;
						if (OutMismatches.Num() < kMaxMismatchesPrinted)
						{
							OutMismatches.Add({ FString::Printf(TEXT("col(%d,%d).%s"),
							                                    int32(Region.OriginVx + X),
							                                    int32(Region.OriginVy + Y), Field),
							                    CpuVal, GpuVal });
						}
					}
				};
				Record(TEXT("surfaceMm"), C.surfaceMm, G.SurfaceMm);
				Record(TEXT("topsoilMm"), C.topsoilMm, G.TopsoilMm);
				Record(TEXT("subsoilMm"), C.subsoilMm, G.SubsoilMm);
				Record(TEXT("bedrockDepthMm"), C.bedrockDepthMm, G.BedrockDepthMm);
				Record(TEXT("surfaceMat"), int64(C.surfaceMat), int64(G.SurfaceMat));

				// This column's vertical cell stack.
				const uint32 Bx = X / 8u, By = Y / 8u, Lx = X % 8u, Ly = Y % 8u;
				const uint32 FootprintIndex = Bx + BricksX * By;
				for (uint32 Bz = 0; Bz < BricksZ; ++Bz)
				{
					const uint32 BrickIndex = FootprintIndex * BricksZ + Bz;
					const int64 BrickZ = int64(Req.BrickZMin) + int64(Bz);
					for (uint32 ZLocal = 0; ZLocal < 8u; ++ZLocal)
					{
						// D5: mirrors makeCoarseBrick's
						// materialAt(col, coarseRep(key.z * B + z, level)).
						const int64 Vz = CoarseRep(BrickZ * 8 + ZLocal, Region.CoarseLevel);
						const uint32 CellIdx = BrickIndex * 512 + CellIndexInBrick(Lx, Ly, ZLocal);
						const uint8 CpuMat = static_cast<uint8>(vxc::Amplifier::materialAt(C, Vz));
						const uint8 GpuMat = static_cast<uint8>(Gpu.Cells[CellIdx] & 0xffu);

						Digest.u8(GpuMat);
						CpuDigest.u8(CpuMat);
						if (CpuMat != GpuMat)
						{
							++Tally.Cells;
							if (OutMismatches.Num() < kMaxMismatchesPrinted)
							{
								OutMismatches.Add({ FString::Printf(TEXT("cell(%d,%d,vz=%lld)"),
								                                    int32(Region.OriginVx + X),
								                                    int32(Region.OriginVy + Y), Vz),
								                    CpuMat, GpuMat });
							}
						}
					}
				}
			}
		}

		// --- quads ----------------------------------------------------------
		const uint32 InteriorX = BricksX - 2;
		const uint32 InteriorY = (Region.Height / 8) - 2;
		const uint32 InteriorZ = BricksZ - 2;

		std::vector<vxc::Quad> CpuQuads;
		uint32 GpuCursor = 0;

		for (uint32 Iz = 0; Iz < InteriorZ; ++Iz)
		{
			for (uint32 Iy = 0; Iy < InteriorY; ++Iy)
			{
				for (uint32 Ix = 0; Ix < InteriorX; ++Ix)
				{
					// Interior brick (ix,iy,iz) is region brick (ix+1,iy+1,iz+1):
					// the +1 skips the halo brick that supplies apron reads.
					const int64 Ox = (int64(Ix) + 1) * 8;
					const int64 Oy = (int64(Iy) + 1) * 8;
					const int64 Oz = (int64(Iz) + 1) * 8;

					const auto Sampler = [&](int Sx, int Sy, int Sz) -> vxc::MaterialId
					{
						const int64 Rvx = Ox + Sx;
						const int64 Rvy = Oy + Sy;
						// D5: the SAME coarse z mapping the cell loop above uses,
						// and the same one makeCoarseBrick applies. Identity at
						// level 0.
						//
						// Leaving this at level-0 z while the columns were coarse
						// did not produce WRONG quads, it produced NONE: at level
						// 1 the coarse brick range maps to z values far below the
						// surface, every sample came back solid, no face had an
						// air neighbour, and the CPU reference was empty. The gate
						// reported "quad count: cpu=0 gpu=3354" -- which reads at a
						// glance like the GPU inventing geometry, and is in fact
						// the reference not being asked for any.
						const int64 Vz = CoarseRep(int64(Req.BrickZMin) * 8 + Oz + Sz,
						                           Region.CoarseLevel);
						const vxc::ColumnSample& C =
							CpuColumns[int32(Rvx + Rvy * int64(Region.Width))];
						return vxc::Amplifier::materialAt(C, Vz);
					};

					CpuQuads.clear();
					vxc::meshBrick<8>(Sampler, CpuQuads);

					for (const vxc::Quad& Q : CpuQuads)
					{
						const uint64 Packed = (GpuCursor < uint32(Gpu.Quads.Num()))
							? Gpu.Quads[int32(GpuCursor)]
							: ~0ull;
						const uint32 W0 = uint32(Packed & 0xffffffffull);
						const uint32 W1 = uint32(Packed >> 32);

						const uint8 GAxis  = uint8(W0 & 0xfu);
						const uint8 GDir   = uint8((W0 >> 4) & 0xfu);
						const uint8 GSlice = uint8((W0 >> 8) & 0xffu);
						const uint8 GU0    = uint8((W0 >> 16) & 0xffu);
						const uint8 GV0    = uint8((W0 >> 24) & 0xffu);
						const uint8 GW     = uint8(W1 & 0xffu);
						const uint8 GH     = uint8((W1 >> 8) & 0xffu);
						const uint8 GAo    = uint8((W1 >> 16) & 0xffu);
						const uint8 GMat   = uint8((W1 >> 24) & 0xffu);

						Digest.u8(GAxis);  Digest.u8(GDir);  Digest.u8(GSlice);
						Digest.u8(GU0);    Digest.u8(GV0);   Digest.u8(GW);
						Digest.u8(GH);     Digest.u8(GAo);   Digest.u8(GMat);

						CpuDigest.u8(Q.axis);  CpuDigest.u8(Q.positive); CpuDigest.u8(Q.slice);
						CpuDigest.u8(Q.u0);    CpuDigest.u8(Q.v0);       CpuDigest.u8(Q.w);
						CpuDigest.u8(Q.h);     CpuDigest.u8(Q.ao);       CpuDigest.u8(uint8(Q.mat));

						const bool bSame = GAxis == Q.axis && GDir == Q.positive
						                && GSlice == Q.slice && GU0 == Q.u0 && GV0 == Q.v0
						                && GW == Q.w && GH == Q.h && GAo == Q.ao
						                && GMat == uint8(Q.mat);
						if (!bSame)
						{
							++Tally.Quads;
							if (OutMismatches.Num() < kMaxMismatchesPrinted)
							{
								OutMismatches.Add({
									FString::Printf(TEXT("quad[%u] brick(%u,%u,%u) cpu(ax%u d%u s%u ")
									                TEXT("u%u v%u w%u h%u ao%u m%u)"),
									                GpuCursor, Ix, Iy, Iz,
									                Q.axis, Q.positive, Q.slice, Q.u0, Q.v0,
									                Q.w, Q.h, Q.ao, uint32(Q.mat)),
									int64(Q.axis), int64(GAxis) });
							}
						}
						++GpuCursor;
					}
				}
			}
		}

		if (GpuCursor != Gpu.NumQuads)
		{
			++Tally.Quads;
			OutMismatches.Add({ TEXT("quad count"), int64(GpuCursor), int64(Gpu.NumQuads) });
		}

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("  [%s] %u columns, %u cells, %u quads (cpu %u)"),
		       Region.Name, Region.Width * Region.Height, Gpu.Cells.Num(), Gpu.NumQuads, GpuCursor);

		return OutMismatches.Num() == 0;
	}

	// --- Wave D / D6: the footprint band gate -------------------------------
	//
	// WHAT IT COMPARES AGAINST, AND WHY THAT IS THE WHOLE POINT.
	// VoxelStreaming::ColumnSurfaceTopVoxel and ColumnDeepestAirVoxel, called
	// directly out of VoxelFootprintBand.h — the SAME functions the level-0
	// worker job reduces its 34x34 column grid with, not a transcription of
	// them. They were lifted out of VoxelWorldSubsystem.cpp specifically so this
	// could call them, exactly the way MeshChunkBricks was lifted so
	// voxel.GPU.VerifyAsyncMesh could compare against the shipping mesher.
	//
	// WHY IT IS NOT ONE PROBE. The band is a max/min over ~1,156 columns, so a
	// per-column error that is not at the extremum is invisible in the reduced
	// answer: one window-shaped probe can pass while the reduction is wrong for
	// most of the world. So the sweep below runs THREE window probes (different
	// origins and sizes — a dropped BandOriginI fails probe 2 but not probe 1)
	// and then SINGLE-COLUMN probes, where BandEdge 1 makes the reduction
	// degenerate and the comparison becomes an exact test of the per-column
	// function itself. The single-column probes are aimed at the columns with
	// the most cave and cavern segments, because those are the branches the
	// kernel had to mirror; a probe over featureless ground exercises only
	// `surfaceTop + 1` and would certify almost nothing.
	//
	// Single-column probes can only sit on the DIAGONAL: BandOriginI is one
	// scalar for both axes (see the .usf — the production window is square, and
	// a square window's max/min is transposition-invariant, so a second origin
	// would only add API surface).
	struct FBandProbe
	{
		uint32 OriginI;
		uint32 OriginJ;
		uint32 Edge;
		FString Why;
	};

	// One probe: reduce the CPU side over exactly the same window and compare.
	bool CompareOneBandProbe(const FRegionSpec& Region,
	                         const FVoxelGpuRegionRequest& BaseReq,
	                         const TArray<vxc::ColumnSample>& CpuColumns,
	                         const FBandProbe& Probe)
	{
		FVoxelGpuRegionRequest Req = BaseReq;
		// The band is a pure function of the columns, so the mesh chain is dead
		// weight here — and dropping it is what makes a multi-probe sweep cheap
		// enough to be worth having.
		Req.bMeshChain = false;
		Req.BandOriginI = Probe.OriginI;
		Req.BandOriginJ = Probe.OriginJ;
		Req.BandEdge = Probe.Edge;

		const FVoxelGpuRegionResult Gpu = VoxelGpuWorldGen::RunRegionBlocking(Req);
		if (!Gpu.bOk)
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("[D6 band cross-check] FAIL — [%s] probe origin=%u edge=%u dispatch failed: %s"),
			       Region.Name, Probe.OriginI, Probe.Edge, *Gpu.Error);
			return false;
		}
		if (!Gpu.bBandValid)
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("[D6 band cross-check] FAIL — [%s] probe origin=%u edge=%u asked for a band ")
			       TEXT("and did not get one. BandReduceMain did not run, or its readback never ")
			       TEXT("landed — either way nothing below was measured."),
			       Region.Name, Probe.OriginI, Probe.Edge);
			return false;
		}

		// The shipping reduction, over the identical window.
		int64 CpuMaxTop = MIN_int64;
		int64 CpuMinAir = MAX_int64;
		int32 CaveCols = 0;
		int32 CavernCols = 0;
		int32 ShaftCols = 0;
		for (uint32 J = 0; J < Probe.Edge; ++J)
		{
			for (uint32 I = 0; I < Probe.Edge; ++I)
			{
				const uint32 Dx = Probe.OriginI + I;
				const uint32 Dy = Probe.OriginJ + J;
				const vxc::ColumnSample& Col = CpuColumns[int32(Dx + Dy * Region.Width)];
				CpuMaxTop = FMath::Max(CpuMaxTop, VoxelStreaming::ColumnSurfaceTopVoxel(Col));
				CpuMinAir = FMath::Min(CpuMinAir, VoxelStreaming::ColumnDeepestAirVoxel(Col));
				CaveCols += (Col.cave.count > 0) ? 1 : 0;
				ShaftCols += (Col.cave.shaftMarginSq > 0) ? 1 : 0;
				CavernCols += (Col.cavern.count > 0) ? 1 : 0;
			}
		}

		const bool bMatch = int64(Gpu.BandMaxSurfaceTopVoxel) == CpuMaxTop
		                 && int64(Gpu.BandMinDeepestAirVoxel) == CpuMinAir;

		// The band as the streaming path would actually hold it. Compared as
		// well as the raw extrema so the widening cannot be the thing that is
		// wrong — MakeFootprintBand is shared with the worker job, so this also
		// pins that the GPU path feeds it the values it expects.
		const VoxelStreaming::FFootprintBand CpuBand =
			VoxelStreaming::MakeFootprintBand(CpuMaxTop, CpuMinAir);
		const VoxelStreaming::FFootprintBand GpuBand =
			VoxelStreaming::MakeFootprintBand(Gpu.BandMaxSurfaceTopVoxel, Gpu.BandMinDeepestAirVoxel);

		if (!bMatch)
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("[D6 band cross-check] FAIL — [%s] %s (origin=%u,%u edge=%u): ")
			       TEXT("maxSurfaceTop gpu %d vs cpu %lld, minDeepestAir gpu %d vs cpu %lld. ")
			       TEXT("Band would be {max %d, solidBelow %d} instead of {max %d, solidBelow %d}. ")
			       TEXT("%d cave / %d shaft / %d cavern column(s) in this window."),
			       Region.Name, *Probe.Why, Probe.OriginI, Probe.OriginJ, Probe.Edge,
			       Gpu.BandMaxSurfaceTopVoxel, CpuMaxTop, Gpu.BandMinDeepestAirVoxel, CpuMinAir,
			       GpuBand.MaxSurfaceTopVoxel, GpuBand.SolidBelowVoxel,
			       CpuBand.MaxSurfaceTopVoxel, CpuBand.SolidBelowVoxel,
			       CaveCols, ShaftCols, CavernCols);

			// WHICH WAY IT IS WRONG IS NOT A DETAIL. A band that claims MORE
			// emptiness than the truth skips chunks that should have been
			// meshed — holes in the world, blamed on streaming. The other
			// direction only wastes work.
			const bool bUnsafeAir = GpuBand.MaxSurfaceTopVoxel < CpuBand.MaxSurfaceTopVoxel;
			const bool bUnsafeSolid = GpuBand.SolidBelowVoxel > CpuBand.SolidBelowVoxel;
			if (bUnsafeAir || bUnsafeSolid)
			{
				UE_LOG(LogVoxelGpuVerify, Error,
				       TEXT("  UNSAFE DIRECTION: the GPU band claims MORE emptiness than the CPU ")
				       TEXT("one (%s%s). This would skip chunks that have geometry."),
				       bUnsafeAir ? TEXT("all-air bound too low") : TEXT(""),
				       bUnsafeSolid ? TEXT(" all-solid bound too high") : TEXT(""));
			}
			else
			{
				UE_LOG(LogVoxelGpuVerify, Error,
				       TEXT("  Conservative direction (wastes work rather than deleting geometry) ")
				       TEXT("— still a failure: the two reductions must be exact."));
			}
			return false;
		}

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("[D6 band cross-check] PASS — [%s] %s (origin=%u,%u edge=%u, %u columns): ")
		       TEXT("maxSurfaceTop %lld, minDeepestAir %lld, band {max %d, solidBelow %d}; ")
		       TEXT("%d cave / %d shaft / %d cavern column(s) exercised"),
		       Region.Name, *Probe.Why, Probe.OriginI, Probe.OriginJ, Probe.Edge, Probe.Edge * Probe.Edge,
		       CpuMaxTop, CpuMinAir, CpuBand.MaxSurfaceTopVoxel, CpuBand.SolidBelowVoxel,
		       CaveCols, ShaftCols, CavernCols);
		return true;
	}

	// Builds the sweep and runs it. Returns false if any probe failed.
	bool VerifyBandForRegion(const FRegionSpec& Region,
	                         const FVoxelGpuRegionRequest& BaseReq,
	                         const TArray<vxc::ColumnSample>& CpuColumns)
	{
		// The production window is 34 (a 32-voxel chunk plus its one-column
		// apron), which fits inside these 64x64 fixtures at any origin up to 30.
		constexpr uint32 kProdEdge = 32 + 2;

		TArray<FBandProbe> Probes;
		Probes.Add({ 0, 0, kProdEdge, TEXT("production-shaped window at origin") });
		Probes.Add({ Region.Width - kProdEdge, Region.Width - kProdEdge, kProdEdge,
		             TEXT("production-shaped window, offset") });
		Probes.Add({ 17, 17, 8, TEXT("small offset window") });
		// Asymmetric window. Only reachable since BandOriginJ existed, and it is
		// the one probe that can catch an x/y mix-up in the kernel's indexing --
		// every window above is square AND diagonal, so a swap of dx and dy is
		// invisible to all of them.
		Probes.Add({ 5, 23, 16, TEXT("asymmetric window (x!=y origin)") });

		// Single-column probes. A featureless column tests only
		// `surfaceTop + 1`; these are what test the cave-segment loop, the
		// sinkhole shaft term, the two bedrock clamps and the integer ceil-sqrt
		// against the CPU's FP-seeded one.
		//
		// HOW THEY ARE CHOSEN, PRECISELY, because the PASS line makes a claim
		// about its own coverage and that claim has to be true:
		//
		//   * The segments are COUNTED, not assumed. The loop below reads
		//     Col.cave.count, Col.cave.shaftMarginSq and Col.cavern.count out of
		//     the real vxc::ColumnSample the CPU reference already built for
		//     this region, scores every candidate, and takes the top three. It
		//     is not a guess that features cluster anywhere in particular.
		//   * The candidates are now EVERY column in the fixture -- all 4,096,
		//     not the 64 on the diagonal. That changed when BandOriginJ was
		//     added: a 1x1 window used to be pinned to (k, k), and the first
		//     real run of this sweep reported the cost of that in its own words
		//     -- no cave or cavern column anywhere on either fixture's diagonal,
		//     so the strongest probes exercised only `surfaceTop + 1` and left
		//     the cave-segment loop, the shaft term, the bedrock clamps and
		//     ceilSqrt uncovered. "The three richest columns in the region" is
		//     now the true claim.
		//   * The three window probes above cover all 4,096 columns between
		//     them. What they cannot do is localise a per-column error, which is
		//     what these three are for.
		//
		// If the diagonal turns out to have no cave or cavern columns at all,
		// the sweep still runs -- but its strongest leg has not fired, and that
		// is said out loud below rather than left to be inferred from three
		// "0 cave / 0 shaft / 0 cavern" PASS lines.
		{
			struct FScored { uint32 I; uint32 J; int32 Score; };
			TArray<FScored> Scored;
			Scored.Reserve(int32(Region.Width) * int32(Region.Height));
			for (uint32 J = 0; J < Region.Height; ++J)
			{
				for (uint32 I = 0; I < Region.Width; ++I)
				{
					const vxc::ColumnSample& Col = CpuColumns[int32(I + J * Region.Width)];
					const int32 Score = Col.cave.count * 2
					                  + ((Col.cave.shaftMarginSq > 0) ? 5 : 0)
					                  + Col.cavern.count * 3;
					Scored.Add({ I, J, Score });
				}
			}
			// Ties broken by K so the gate probes the SAME columns every run.
			// TArray::Sort is introsort and is not stable; without this, two
			// runs of the same binary on the same fixture could report coverage
			// from different columns, and a gate whose scope moves between runs
			// is not a gate.
			// Ties broken by (J, I) so the gate probes the SAME columns every
			// run. TArray::Sort is introsort and is not stable; without this,
			// two runs of the same binary on the same fixture could report
			// coverage from different columns, and a gate whose scope moves
			// between runs is not a gate.
			Scored.Sort([](const FScored& A, const FScored& B)
			{
				if (A.Score != B.Score) { return A.Score > B.Score; }
				if (A.J != B.J)         { return A.J < B.J; }
				return A.I < B.I;
			});

			if (Scored.Num() > 0 && Scored[0].Score == 0)
			{
				UE_LOG(LogVoxelGpuVerify, Warning,
				       TEXT("[D6 band cross-check] [%s] NO cave or cavern column ANYWHERE in this ")
				       TEXT("region's %u columns, so the single-column probes below exercise only ")
				       TEXT("`surfaceTop + 1`. The cave-segment loop, the shaft term, the bedrock ")
				       TEXT("clamps and ceilSqrt are NOT covered by this region's probes -- read ")
				       TEXT("their PASS lines accordingly. (Before BandOriginJ this warning could ")
				       TEXT("fire merely because the DIAGONAL was featureless; now it means the ")
				       TEXT("whole fixture is.)"),
				       Region.Name, Region.Width * Region.Height);
			}

			for (int32 N = 0; N < 3 && N < Scored.Num(); ++N)
			{
				Probes.Add({ Scored[N].I, Scored[N].J, 1,
				             FString::Printf(TEXT("single column (%d,%d), richest-in-region rank %d, score %d"),
				                             Region.OriginVx + int32(Scored[N].I),
				                             Region.OriginVy + int32(Scored[N].J),
				                             N + 1, Scored[N].Score) });
			}
		}

		bool bOk = true;
		for (const FBandProbe& Probe : Probes)
		{
			bOk &= CompareOneBandProbe(Region, BaseReq, CpuColumns, Probe);
		}
		return bOk;
	}

	// CPU reference for the packed-quad decode.
	//
	// SCOPE, HONESTLY STATED: this is transcribed from BuildChunkVertexData in
	// VoxelChunkComponent.cpp, it is not that function. So it proves the shader
	// implements the SPEC — the bit unpacking, the corner order, the winding
	// flip, the AO bit remap, the level scale — which is where transcription
	// bugs actually live. It does NOT prove the GPU path matches the shipping
	// CPU renderer; only the G2 visual A/B against real terrain does that, and
	// that check is still owed.
	void DecodeQuadVertexCpu(uint64 Packed, uint32 CornerIndex, float LevelScale,
	                         float OutPosUU[3], uint32& OutAo, uint32& OutMat)
	{
		const uint32 W0 = uint32(Packed & 0xffffffffull);
		const uint32 W1 = uint32(Packed >> 32);

		const uint32 Axis     =  W0        & 0xfu;
		const uint32 Positive = (W0 >>  4) & 0xfu;
		const uint32 Slice    = (W0 >>  8) & 0xffu;
		const uint32 U0       = (W0 >> 16) & 0xffu;
		const uint32 V0       = (W0 >> 24) & 0xffu;
		const uint32 QW       =  W1        & 0xffu;
		const uint32 QH       = (W1 >>  8) & 0xffu;
		const uint32 Ao       = (W1 >> 16) & 0xffu;
		const uint32 Mat      = (W1 >> 24) & 0xffu;

		const uint32 U = (Axis + 1u) % 3u;
		const uint32 V = (Axis + 2u) % 3u;
		const float FaceCoordVox = float(Slice) + (Positive != 0u ? 1.0f : 0.0f);

		const float Us[4] = { float(U0), float(U0), float(U0 + QW), float(U0 + QW) };
		const float Vs[4] = { float(V0), float(V0 + QH), float(V0 + QH), float(V0) };

		static const uint32 ForwardWinding[6]  = { 0u, 1u, 2u, 0u, 2u, 3u };
		static const uint32 ReversedWinding[6] = { 0u, 2u, 1u, 0u, 3u, 2u };
		const uint32 Corner = (Positive != 0u) ? ForwardWinding[CornerIndex]
		                                       : ReversedWinding[CornerIndex];

		float PosVox[3] = { 0.f, 0.f, 0.f };
		PosVox[Axis] = FaceCoordVox;
		PosVox[U] = Us[Corner];
		PosVox[V] = Vs[Corner];

		// VoxelCoords::VoxelSizeUU
		constexpr float VoxelSizeUU = 10.0f;
		for (int32 I = 0; I < 3; ++I)
		{
			OutPosUU[I] = PosVox[I] * (VoxelSizeUU * LevelScale);
		}

		static const uint32 AoShiftForCorner[4] = { 0u, 4u, 6u, 2u };
		OutAo = (Ao >> AoShiftForCorner[Corner]) & 0x3u;
		OutMat = Mat;
	}

	// Compares the GPU decode against the CPU reference over a real quad
	// stream. Returns the number of mismatching vertices.
	int32 VerifyQuadDecode(const TArray<uint64>& Quads, float LevelScale)
	{
		TArray<VoxelGpuWorldGen::FDecodedVertex> GpuVerts;
		FString Error;
		if (!VoxelGpuWorldGen::DecodeQuadsBlocking(Quads, LevelScale, GpuVerts, Error))
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("  quad decode dispatch FAILED: %s"), *Error);
			return -1;
		}

		const int32 Expected = Quads.Num() * 6;
		if (GpuVerts.Num() != Expected)
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("  quad decode produced %d vertices, expected %d"),
			       GpuVerts.Num(), Expected);
			return -1;
		}

		int32 Mismatches = 0;
		for (int32 QuadIdx = 0; QuadIdx < Quads.Num(); ++QuadIdx)
		{
			for (uint32 Corner = 0; Corner < 6; ++Corner)
			{
				float CpuPos[3];
				uint32 CpuAo = 0, CpuMat = 0;
				DecodeQuadVertexCpu(Quads[QuadIdx], Corner, LevelScale, CpuPos, CpuAo, CpuMat);

				const VoxelGpuWorldGen::FDecodedVertex& G = GpuVerts[QuadIdx * 6 + int32(Corner)];

				// Positions are small integers times a power-of-two scale, so
				// they are exactly representable and an exact compare is
				// correct here — no epsilon, which would hide a real error.
				const bool bSame = G.PositionUU[0] == CpuPos[0]
				                && G.PositionUU[1] == CpuPos[1]
				                && G.PositionUU[2] == CpuPos[2]
				                && G.AmbientOcclusion == CpuAo
				                && G.MaterialId == CpuMat;
				if (!bSame)
				{
					if (Mismatches < 10)
					{
						UE_LOG(LogVoxelGpuVerify, Error,
						       TEXT("    quad %d corner %u: cpu (%.1f, %.1f, %.1f) ao=%u mat=%u ")
						       TEXT("vs gpu (%.1f, %.1f, %.1f) ao=%u mat=%u"),
						       QuadIdx, Corner, CpuPos[0], CpuPos[1], CpuPos[2], CpuAo, CpuMat,
						       G.PositionUU[0], G.PositionUU[1], G.PositionUU[2],
						       G.AmbientOcclusion, G.MaterialId);
					}
					++Mismatches;
				}
			}
		}
		return Mismatches;
	}

	void VerifyRegionCommand(const TArray<FString>& Args)
	{
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("GPU worldgen needs SM6 (64-bit integer shader ops). Current feature ")
			       TEXT("level is lower. Relaunch the editor with -sm6, and make sure ")
			       TEXT("+D3D12TargetedShaderFormats=PCD3D_SM6 is in DefaultEngine.ini."));
			return;
		}

		// No argument runs both fixtures, which is the only mode whose digest
		// is comparable to the bench's.
		int32 Only = -1;
		if (Args.Num() > 0)
		{
			Only = FCString::Atoi(*Args[0]);
			if (Only < 0 || Only >= UE_ARRAY_COUNT(kRegions))
			{
				UE_LOG(LogVoxelGpuVerify, Error, TEXT("Region index must be 0..%d"),
				       int32(UE_ARRAY_COUNT(kRegions)) - 1);
				return;
			}
		}

		vxc::SyntheticTileSampler Tiles(kSeed);

		// A second, independent sampler for the CPU reference — the bench does
		// the same. Sharing one would let a stateful bug in the sampler cancel
		// itself out on both sides of the comparison.
		vxc::SyntheticTileSampler CpuTiles(kSeed);
		vxc::Amplifier CpuAmp(kSeed, CpuTiles);

		vxc::Digest Digest;
		vxc::Digest CpuDigest;
		FTally Tally;
		TArray<FMismatch> Mismatches;
		bool bAllOk = true;
		double TotalMs = 0.0;

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("voxel.GPU.VerifyRegion: running the worldgen+mesher chain through Unreal's ")
		       TEXT("RDG, seed %llu"), kSeed);

		for (int32 R = 0; R < UE_ARRAY_COUNT(kRegions); ++R)
		{
			if (Only >= 0 && R != Only)
			{
				continue;
			}
			const FRegionSpec& Region = kRegions[R];

			TArray<vxc::ColumnSample> CpuColumns;
			const FVoxelGpuRegionRequest Req = BuildRequest(Region, CpuAmp, Tiles, CpuColumns);

			const double Start = FPlatformTime::Seconds();
			const FVoxelGpuRegionResult Gpu = VoxelGpuWorldGen::RunRegionBlocking(Req);
			const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
			TotalMs += Ms;

			if (!Gpu.bOk)
			{
				UE_LOG(LogVoxelGpuVerify, Error, TEXT("  [%s] dispatch FAILED: %s"),
				       Region.Name, *Gpu.Error);
				bAllOk = false;
				continue;
			}

			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("  [%s] origin (%d,%d) %ux%u columns, raster %ux%u px, bricks z [%d, %d], %.1f ms"),
			       Region.Name, Region.OriginVx, Region.OriginVy, Region.Width, Region.Height,
			       Req.RasterSize.X, Req.RasterSize.Y, Req.BrickZMin,
			       Req.BrickZMin + int32(Req.BricksZ) - 1, Ms);

			bAllOk &= CompareRegion(Region, Req, Gpu, CpuColumns, Digest, CpuDigest, Tally,
			                        Mismatches);

			// G2 decode check, over this region's real quad stream. Level 0,
			// so LevelScale is 1.
			const int32 DecodeMismatches = VerifyQuadDecode(Gpu.Quads, 1.0f);
			if (DecodeMismatches < 0)
			{
				bAllOk = false;
			}
			else if (DecodeMismatches > 0)
			{
				UE_LOG(LogVoxelGpuVerify, Error,
				       TEXT("  [%s] quad decode: %d of %d vertices differ from the CPU reference"),
				       Region.Name, DecodeMismatches, Gpu.Quads.Num() * 6);
				bAllOk = false;
			}
			else
			{
				UE_LOG(LogVoxelGpuVerify, Log,
				       TEXT("  [%s] quad decode: %d vertices match the CPU reference exactly"),
				       Region.Name, Gpu.Quads.Num() * 6);
			}

			// Wave D / D4. UnpackVoxelChunkQuad is what lets a GPU-meshed chunk
			// feed the GI light field, which takes FVoxelChunkQuad and not the
			// packed word. Checked HERE, against the real GPU quad stream,
			// rather than in a unit test over synthetic values: the failure
			// that matters is a field the packer and unpacker disagree about on
			// geometry that actually occurs, and this region has 3,000+ of it.
			// The round trip is lossless by construction, so any mismatch is a
			// transcription error in one of the two shift tables.
			{
				int32 RoundTripMismatches = 0;
				for (int32 QI = 0; QI < Gpu.Quads.Num(); ++QI)
				{
					const uint64 Packed = Gpu.Quads[QI];
					if (PackVoxelChunkQuad(UnpackVoxelChunkQuad(Packed)) != Packed)
					{
						if (RoundTripMismatches < 5)
						{
							UE_LOG(LogVoxelGpuVerify, Error,
							       TEXT("    quad %d: 0x%016llx -> unpack -> repack -> 0x%016llx"),
							       QI, Packed, PackVoxelChunkQuad(UnpackVoxelChunkQuad(Packed)));
						}
						++RoundTripMismatches;
					}
				}
				if (RoundTripMismatches > 0)
				{
					UE_LOG(LogVoxelGpuVerify, Error,
					       TEXT("[D4 quad pack round-trip] FAIL — [%s] %d of %d quads do not survive ")
					       TEXT("UnpackVoxelChunkQuad -> PackVoxelChunkQuad. A GPU-meshed chunk feeds ")
					       TEXT("the GI light field through that inverse, so this is silent wrong ")
					       TEXT("lighting, not a draw fault."),
					       Region.Name, RoundTripMismatches, Gpu.Quads.Num());
					bAllOk = false;
				}
				else
				{
					UE_LOG(LogVoxelGpuVerify, Log,
					       TEXT("[D4 quad pack round-trip] PASS — [%s] all %d quads survive ")
					       TEXT("unpack->repack byte-identically"),
					       Region.Name, Gpu.Quads.Num());
				}
			}

			// Wave D / D6. Its own PASS/FAIL lines, like D3's quad-total
			// cross-check, because a band is not part of the digest and a
			// failure here means something different from a cell mismatch:
			// the geometry is right and the decision to SKIP producing it is
			// wrong.
			bAllOk &= VerifyBandForRegion(Region, Req, CpuColumns);
		}

		// Band-only fixtures, AFTER the digest loop and outside it.
		//
		// Nothing here touches Digest or CpuDigest, which is the point: the
		// pinned 6e893ab3679a8c81 is a cross-toolchain gate against the bench,
		// and widening its fixture set would silently re-baseline it. These
		// exist because the two digest fixtures are 6.4 m across against a
		// 25.6 m cave lattice and contain no cave sites at all, so the band
		// kernel's cave path was passing vacuously.
		//
		// Skipped when a single region was requested by index, since the
		// argument selects from kRegions.
		if (Only < 0)
		{
			for (const FRegionSpec& BandRegion : kBandOnlyRegions)
			{
				TArray<vxc::ColumnSample> BandCpuColumns;
				FVoxelGpuRegionRequest BandReq =
					BuildRequest(BandRegion, CpuAmp, Tiles, BandCpuColumns);
				// The band is a pure function of the columns, so the mesh chain
				// is dead weight here -- and at 320x320 it would be expensive
				// dead weight.
				BandReq.bMeshChain = false;

				UE_LOG(LogVoxelGpuVerify, Log,
				       TEXT("  [%s] band-only fixture, origin (%d,%d) %ux%u columns "
				            "(%u total) -- does NOT contribute to the digest"),
				       BandRegion.Name, BandRegion.OriginVx, BandRegion.OriginVy,
				       BandRegion.Width, BandRegion.Height,
				       BandRegion.Width * BandRegion.Height);

				bAllOk &= VerifyBandForRegion(BandRegion, BandReq, BandCpuColumns);
			}
		}

		// vxc::Digest exposes its FNV-1a accumulator directly as `h`.
		const uint64 Value = Digest.h;
		const uint64 CpuValue = CpuDigest.h;

		// WHICH SIDE MOVED. Run this before printing anything about the GPU.
		//
		// The CPU digest is a pure function of the LINKED voxel-core, over
		// fixtures this file pins itself. If it is not the pinned value then
		// the reference this gate compares against is not the one the kernel
		// mirrors, and every per-cell "cpu=N gpu=M" line below is a version
		// skew, not a toolchain divergence. That is exactly what happened in
		// July 2026: voxelcore.lib predated the worldgen v8 CPU landing while
		// Unreal compiled the current worldgen.ush, and the resulting cell
		// mismatches were published twice as a shader-compilation fault.
		//
		// Only meaningful over BOTH fixture regions, so it is skipped when a
		// single region was requested.
		bool bCpuReferenceSuspect = false;
		if (Only < 0)
		{
			if (CpuValue != kExpectedCpuDigest)
			{
				bCpuReferenceSuspect = true;
				bAllOk = false;
				UE_LOG(LogVoxelGpuVerify, Error,
				       TEXT("CPU REFERENCE DIGEST MISMATCH: got %016llx, expected %016llx ")
				       TEXT("(vxc::kWorldGenVersion = %u). The linked voxelcore.lib is NOT the ")
				       TEXT("worldgen this gate is pinned to — rebuild it before reading ")
				       TEXT("anything below:  cmake --build build/voxel-core-msvc --config Release"),
				       CpuValue, kExpectedCpuDigest, vxc::kWorldGenVersion);
			}
			else
			{
				UE_LOG(LogVoxelGpuVerify, Log,
				       TEXT("CPU reference digest: %016llx (matches the pinned value; ")
				       TEXT("vxc::kWorldGenVersion = %u)"), CpuValue, vxc::kWorldGenVersion);
			}
		}

		if (Tally.Total() > 0)
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("Mismatches by stage: %lld column field(s), %lld cell(s), %lld quad(s)."),
			       Tally.Columns, Tally.Cells, Tally.Quads);
			if (Tally.Columns > 0)
			{
				UE_LOG(LogVoxelGpuVerify, Error,
				       TEXT("  COLUMN FIELDS DIFFER. ColumnMain and vxc::Amplifier::column ")
				       TEXT("disagree, so this is not a mesher or voxelize fault. If the CPU ")
				       TEXT("reference digest above also failed, it is a stale voxelcore.lib; ")
				       TEXT("otherwise worldgen.ush has drifted from the CPU worldgen and needs ")
				       TEXT("re-mirroring."));
			}
			else if (Tally.Cells > 0)
			{
				UE_LOG(LogVoxelGpuVerify, Error,
				       TEXT("  CELLS ONLY. Columns agree, so ColumnMain is fine and the fault is ")
				       TEXT("in VoxelizeMain's stratigraphy/cave/cavern path or in the ")
				       TEXT("BrickZMin/BricksZ it received."));
			}
			if (bCpuReferenceSuspect)
			{
				UE_LOG(LogVoxelGpuVerify, Error,
				       TEXT("  Do NOT diagnose the shader toolchain until the CPU reference ")
				       TEXT("digest above matches."));
			}
		}

		if (!Mismatches.IsEmpty())
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("First %d mismatch(es) (in comparison order — ")
			       TEXT("column fields precede that column's cells; do not quote a subset):"),
			       Mismatches.Num());
			for (const FMismatch& M : Mismatches)
			{
				UE_LOG(LogVoxelGpuVerify, Error, TEXT("    %s: cpu=%lld gpu=%lld"),
				       *M.Where, M.Cpu, M.Gpu);
			}
		}

		if (bAllOk)
		{
			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("PASS: Unreal-compiled GPU output is bit-exact with the CPU reference ")
			       TEXT("(%.1f ms total)"), TotalMs);
		}
		else if (Tally.Total() == 0 && bCpuReferenceSuspect)
		{
			// The two sides agree with each other and disagree with the pinned
			// baseline — i.e. both moved together. Say so, because "GPU differs
			// from the CPU reference" would be a false statement here.
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("FAIL: GPU and CPU agree with each other but not with the pinned ")
			       TEXT("worldgen baseline — the whole reference moved, not one leg of it"));
		}
		else
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("FAIL: Unreal-compiled GPU output differs from the CPU reference"));
		}

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("Unreal GPU output digest (columns + cells + quads, combined): %016llx"), Value);

		if (Only < 0)
		{
			// Both fixtures, bench order. This is the cross-toolchain gate:
			// vxc_gpu.exe with no arguments must print this same number.
			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("Compare against: build/voxel-core-msvc/bench/vxc_gpu.exe (no args) — ")
			       TEXT("the digests must match exactly."));
		}
	}

	// Finds a band fixture that actually contains the feature you need.
	//
	// WHY THIS EXISTS RATHER THAN A HAND-PICKED COORDINATE. Choosing fixtures by
	// eye is how the D6 sweep spent its whole life reporting "0 cave / 0 shaft /
	// 0 cavern": the two digest fixtures are 6.4 m across against a 25.6 m cave
	// lattice. Widening them to 32 m fixed CAVES, and then five hand-aligned
	// anchors covering 512,000 columns still found ZERO shafts and ZERO caverns
	// -- which is exactly what the constants predict. Shaft nodes sit 1024
	// voxels apart and only 1 IN 4 opens (kCaveShaftNodeMask 3,
	// kCaveShaftGateMask 3), so a 320-column window contains an open shaft about
	// 2% of the time. Guessing is not a method; this searches.
	//
	// CPU only -- no dispatch, no RHI. It walks vxc::ColumnSample directly, so
	// it is cheap enough to sweep kilometres and can run on any machine.
	void FindBandFixtureCommand(const TArray<FString>& Args)
	{
		const int32 HalfSpanVoxels = (Args.Num() > 0) ? FMath::Max(512, FCString::Atoi(*Args[0])) : 8192;
		const int32 Step = (Args.Num() > 1) ? FMath::Max(1, FCString::Atoi(*Args[1])) : 64;

		vxc::SyntheticTileSampler Tiles(kSeed);
		vxc::Amplifier Amp(kSeed, Tiles);

		int64 BestShaftX = 0, BestShaftY = 0; bool bFoundShaft = false;
		int64 BestCavernX = 0, BestCavernY = 0; bool bFoundCavern = false;
		int64 Sampled = 0;

		for (int64 Y = -HalfSpanVoxels; Y <= HalfSpanVoxels && !(bFoundShaft && bFoundCavern); Y += Step)
		{
			for (int64 X = -HalfSpanVoxels; X <= HalfSpanVoxels; X += Step)
			{
				const vxc::ColumnSample C = Amp.column(X, Y);
				++Sampled;
				if (!bFoundShaft && C.cave.shaftMarginSq > 0)
				{
					BestShaftX = X; BestShaftY = Y; bFoundShaft = true;
				}
				if (!bFoundCavern && C.cavern.count > 0)
				{
					BestCavernX = X; BestCavernY = Y; bFoundCavern = true;
				}
				if (bFoundShaft && bFoundCavern) { break; }
			}
		}

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("[FindBandFixture] sampled %lld columns on a %d-voxel grid over +/-%d voxels, seed %llu"),
		       Sampled, Step, HalfSpanVoxels, kSeed);

		// A COARSE GRID CAN MISS. Step is 64 voxels by default and a shaft is
		// ~1.4 m (14 voxels) wide, so a miss is not proof of absence -- it is
		// proof this sweep did not land on one. Said out loud so a "not found"
		// is never read as "does not exist".
		if (bFoundShaft)
		{
			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("[FindBandFixture] SHAFT column at voxel (%lld, %lld) -- a 320-wide fixture ")
			       TEXT("centred there has origin (%lld, %lld)"),
			       BestShaftX, BestShaftY, BestShaftX - 160, BestShaftY - 160);
		}
		else
		{
			UE_LOG(LogVoxelGpuVerify, Warning,
			       TEXT("[FindBandFixture] no shaft column found. The grid step (%d voxels) is wider ")
			       TEXT("than a shaft (~14 voxels), so this is a MISS, not an absence -- re-run with ")
			       TEXT("a smaller step or a larger span."), Step);
		}
		// D5.2 LEFT LEVEL 5 UNPROVEN, AND THIS IS WHY IT COULD NOT BE PROVEN.
		//
		// voxel.GPU.VerifyCoarse passes L0-L4 on both digest fixtures and then
		// L5's [far-negative] dispatch is REFUSED: "Mesh chain needs >= 3 bricks
		// per axis (have 8, 8, 2)". At level 5 a coarse cell is 32 level-0
		// voxels, so a 64-cell region spans 204.8 m laterally and the whole
		// terrain range inside it collapses into 2 coarse bricks of z. The halo
		// then leaves no interior brick to mesh. That is the FIXTURE running out
		// of vertical relief, not the coarse path failing.
		//
		// Three bricks of coarse z needs 3 * 8 * 32 = 768 level-0 voxels of
		// surface spread -- 76.8 m -- inside a 204.8 m window. So: find the
		// most mountainous 64-cell-at-L5 window in the search span and report
		// its origin, rather than guessing at coordinates that "look alpine".
		int64 BestReliefX = 0, BestReliefY = 0, BestRelief = -1;
		{
			// Sample the window corners-and-centre grid coarsely: full coverage
			// of every candidate window would be O(span^2 * window^2), and the
			// point is to LOCATE relief, not to measure it exactly. The gate
			// itself is what decides whether the found fixture works.
			constexpr int64 kL5CellVoxels = 32;
			constexpr int64 kWindowCells = 64;
			const int64 WindowVoxels = kL5CellVoxels * kWindowCells;      // 2048
			const int64 WindowStep = WindowVoxels / 2;
			for (int64 Y = -HalfSpanVoxels; Y + WindowVoxels <= HalfSpanVoxels; Y += WindowStep)
			{
				for (int64 X = -HalfSpanVoxels; X + WindowVoxels <= HalfSpanVoxels; X += WindowStep)
				{
					int64 Lo = MAX_int64, Hi = MIN_int64;
					for (int64 J = 0; J <= kWindowCells; J += 8)
					{
						for (int64 I = 0; I <= kWindowCells; I += 8)
						{
							const vxc::ColumnSample C =
								Amp.column(X + I * kL5CellVoxels, Y + J * kL5CellVoxels);
							Lo = FMath::Min<int64>(Lo, C.surfaceMm);
							Hi = FMath::Max<int64>(Hi, C.surfaceMm);
						}
					}
					const int64 ReliefVoxels = (Hi - Lo) / vxc::kVoxelSizeMm;
					if (ReliefVoxels > BestRelief)
					{
						BestRelief = ReliefVoxels; BestReliefX = X; BestReliefY = Y;
					}
				}
			}
		}
		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("[FindBandFixture] most vertical relief in a 64-cell L5 window: %lld voxels "
		            "(%.0f m) at origin voxel (%lld, %lld). L5 needs >= 768 voxels (76.8 m) for "
		            "three coarse bricks of z; below that the mesh chain refuses the dispatch and "
		            "the level is UNPROVEN rather than broken."),
		       BestRelief, double(BestRelief) * 0.1, BestReliefX, BestReliefY);

		if (bFoundCavern)
		{
			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("[FindBandFixture] CAVERN column at voxel (%lld, %lld) -- a 320-wide fixture ")
			       TEXT("centred there has origin (%lld, %lld)"),
			       BestCavernX, BestCavernY, BestCavernX - 160, BestCavernY - 160);
		}
		else
		{
			UE_LOG(LogVoxelGpuVerify, Warning,
			       TEXT("[FindBandFixture] no cavern column found in the span searched."));
		}
	}

	// D5.2: the per-level gate. Bit-exactness of a COARSE level against
	// vxc::coarseColumns + makeCoarseBrick, one level at a time.
	//
	// WHY IT IS A SEPARATE COMMAND WITH ITS OWN DIGESTS, and this is the part
	// that must not be "simplified" later. 6e893ab3679a8c81 is a LEVEL-0
	// statement: it is what the DXC->SPIR-V->Vulkan bench produces and what the
	// UE->DXIL->D3D12 path must reproduce. Folding a coarse run into it would
	// change the value and turn a loud cross-toolchain gate into a
	// re-baselining exercise -- the exact failure the roadmap already records a
	// standing instruction against. So this compares GPU against CPU directly
	// and pins nothing.
	//
	// SHIP LEVEL BY LEVEL. A level that fails here is not enabled, and the
	// correct response is to stop at the last level that passes rather than
	// relax the comparison.
	void VerifyCoarseCommand(const TArray<FString>& Args)
	{
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("GPU worldgen needs SM6. Relaunch with -sm6."));
			return;
		}

		const int32 MaxLevel = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 0, 5) : 5;

		vxc::SyntheticTileSampler Tiles(kSeed);
		vxc::SyntheticTileSampler CpuTiles(kSeed);
		vxc::Amplifier CpuAmp(kSeed, CpuTiles);

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("voxel.GPU.VerifyCoarse: levels 0..%d against vxc::coarseColumns + ")
		       TEXT("makeCoarseBrick, seed %llu. Level 0 is included deliberately -- it is the ")
		       TEXT("control, and coarseRep makes it the identity, so a failure THERE means the ")
		       TEXT("harness is wrong rather than the coarse path."),
		       MaxLevel, kSeed);

		int32 LastGoodLevel = -1;
		for (int32 Level = 0; Level <= MaxLevel; ++Level)
		{
			bool bLevelOk = true;
			// A level is only proven if a fixture actually EXERCISED it. Without
			// this, a level every fixture skipped would pass vacuously -- the
			// same shape as the D6 band sweep passing twelve probes over terrain
			// containing no caves.
			bool bAnyFixturePassed = false;
			// The two digest fixtures, then the coarse-only extras. The extras
			// are what make level 5 reachable at all -- see kCoarseExtraRegions.
			const int32 NumRegions = UE_ARRAY_COUNT(kRegions) + UE_ARRAY_COUNT(kCoarseExtraRegions);
			for (int32 R = 0; R < NumRegions; ++R)
			{
				FRegionSpec Region = (R < UE_ARRAY_COUNT(kRegions))
					? kRegions[R]
					: kCoarseExtraRegions[R - UE_ARRAY_COUNT(kRegions)];
				Region.CoarseLevel = Level;

				TArray<vxc::ColumnSample> CpuColumns;
				const FVoxelGpuRegionRequest Req = BuildRequest(Region, CpuAmp, Tiles, CpuColumns);
				const FVoxelGpuRegionResult Gpu = VoxelGpuWorldGen::RunRegionBlocking(Req);
				if (!Gpu.bOk)
				{
					// A FIXTURE TOO THIN TO EXPRESS THIS LEVEL IS NOT A LEVEL
					// FAILURE, and conflating the two cost level 5 its verdict
					// once already.
					//
					// At level L a coarse cell is 2^L level-0 voxels, so a
					// 64-cell region spans 64 * 2^L and the terrain range inside
					// it collapses as L rises. Below three coarse bricks of z the
					// halo leaves no interior brick and the mesh chain REFUSES
					// the dispatch. That is the fixture running out of vertical
					// relief; the coarse path never got to disagree about
					// anything. Reported as SKIPPED, and the level still has to
					// be proven by at least one fixture that CAN express it --
					// see bAnyFixturePassed.
					if (Gpu.Error.Contains(TEXT("Mesh chain needs")))
					{
						UE_LOG(LogVoxelGpuVerify, Warning,
						       TEXT("[D5 coarse] SKIP — L%d [%s]: fixture cannot express this level "
						            "(%s). Not a failure of the coarse path; find a fixture with more "
						            "vertical relief via voxel.GPU.FindBandFixture."),
						       Level, Region.Name, *Gpu.Error);
						continue;
					}
					UE_LOG(LogVoxelGpuVerify, Error,
					       TEXT("[D5 coarse] L%d [%s] dispatch FAILED: %s"),
					       Level, Region.Name, *Gpu.Error);
					bLevelOk = false;
					continue;
				}

				// Local digests, deliberately discarded -- CompareRegion needs
				// somewhere to fold bytes and the pinned value must not see them.
				vxc::Digest ScratchGpu, ScratchCpu;
				FTally Tally;
				TArray<FMismatch> Mismatches;
				const bool bOk = CompareRegion(Region, Req, Gpu, CpuColumns,
				                               ScratchGpu, ScratchCpu, Tally, Mismatches);
				bLevelOk &= bOk;

				if (bOk)
				{
					bAnyFixturePassed = true;
					UE_LOG(LogVoxelGpuVerify, Log,
					       TEXT("[D5 coarse] PASS — L%d [%s]: %u columns, %d quads, cell stack ")
					       TEXT("%u bricks deep — bit-exact with coarseColumns + makeCoarseBrick"),
					       Level, Region.Name, Region.Width * Region.Height,
					       Gpu.Quads.Num(), Req.BricksZ);
				}
				else
				{
					// WHICH STAGE disagreed is the whole diagnostic. Columns
					// wrong means the xy mapping (coarseColumns); cells-only
					// wrong means the z mapping (makeCoarseBrick); quads-only
					// means the mesher read a grid it agreed with and still
					// produced different geometry.
					UE_LOG(LogVoxelGpuVerify, Error,
					       TEXT("[D5 coarse] FAIL — L%d [%s]: %lld column, %lld cell, %lld quad ")
					       TEXT("mismatch(es). Columns wrong => the xy mapping; cells only => the ")
					       TEXT("z mapping; quads only => the mesher."),
					       Level, Region.Name, Tally.Columns, Tally.Cells, Tally.Quads);
					for (int32 M = 0; M < FMath::Min(Mismatches.Num(), 10); ++M)
					{
						UE_LOG(LogVoxelGpuVerify, Error, TEXT("    %s: cpu=%lld gpu=%lld"),
						       *Mismatches[M].Where, Mismatches[M].Cpu, Mismatches[M].Gpu);
					}
				}
			}

			if (bLevelOk && !bAnyFixturePassed)
			{
				UE_LOG(LogVoxelGpuVerify, Error,
				       TEXT("[D5 coarse] L%d: EVERY fixture skipped it, so nothing was compared. "
				            "The level is UNPROVEN, not passing."), Level);
				bLevelOk = false;
			}

			if (bLevelOk)
			{
				LastGoodLevel = Level;
			}
			else
			{
				UE_LOG(LogVoxelGpuVerify, Error,
				       TEXT("[D5 coarse] level %d FAILED — stopping. Ship voxel.Stream.GPUMaxLevel ")
				       TEXT("at %d, and do NOT relax this gate to get past it."),
				       Level, LastGoodLevel);
				break;
			}
		}

		if (LastGoodLevel < 0)
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("[D5 coarse] NO level passed, not even 0. Level 0 is the identity in "
			            "coarseRep, so this is the harness, not the coarse path."));
		}
		else
		{
			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("[D5 coarse] highest bit-exact level: %d (of %d requested)"),
			       LastGoodLevel, MaxLevel);
		}
	}

	FAutoConsoleCommand GVoxelGpuVerifyCoarseCmd(
		TEXT("voxel.GPU.VerifyCoarse"),
		TEXT("D5: byte-compare GPU coarse generation against vxc::coarseColumns + makeCoarseBrick, ")
		TEXT("level by level, stopping at the first failure. Does NOT touch the pinned level-0 ")
		TEXT("digest. Usage: [maxLevel=5]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyCoarseCommand));

	FAutoConsoleCommand GVoxelGpuFindBandFixtureCmd(
		TEXT("voxel.GPU.FindBandFixture"),
		TEXT("CPU-only scan for a column containing a sinkhole shaft and one containing a cavern, ")
		TEXT("so a D6 band fixture can be placed where the feature actually is instead of guessed. ")
		TEXT("Usage: [halfSpanVoxels=8192] [stepVoxels=64]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FindBandFixtureCommand));

	FAutoConsoleCommand GVoxelGpuVerifyRegionCmd(
		TEXT("voxel.GPU.VerifyRegion"),
		TEXT("Run the GPU worldgen+mesher chain through Unreal's RDG and byte-compare it against ")
		TEXT("the CPU reference. No args = both bench fixture regions (digest comparable to ")
		TEXT("vxc_gpu.exe); 0 or 1 = a single region."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyRegionCommand));
}

// ---------------------------------------------------------------------------
// voxel.GPU.SpawnTestChunk — the G2 visual check
//
// Everything up to here is verified numerically: the kernels are bit-exact vs
// the CPU mesher, and the quad decode matches a CPU reference exactly. What no
// number can answer is whether the DRAW path works -- whether the geometry is
// lit, shaded, oriented and wound correctly once it reaches the renderer.
//
// So this meshes a region on the GPU and hangs the result in the air in front
// of the player, drawn entirely by FVoxelQuadVertexFactory: no vertex buffer,
// no index buffer, every corner rebuilt from SV_VertexID.
//
// It is deliberately floating and offset rather than sitting in the terrain --
// against open sky a wrong winding or a flipped normal is obvious, whereas
// buried in the landscape it would just look like more ground.
// ---------------------------------------------------------------------------

#include "VoxelGpuChunkComponent.h"
#include "VoxelGpuPoolComponent.h"
#include "VoxelClimateProbe.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "DrawDebugHelpers.h"
#include "VoxelChunkComponent.h"
#include "VoxelMeshTypes.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "UnrealClient.h"
#include "TimerManager.h"

namespace
{
	// Re-bases packed quads from BRICK-LOCAL to region-local coordinates.
	//
	// greedyMask packs slice/u0/v0 inside a single 8x8x8 brick and leaves the
	// brick's origin implied by the mask index, so the raw stream piles all
	// bricks into one 8-voxel cube. The CPU mesher does this same re-basing
	// when it converts vxc::Quad to FVoxelChunkQuad.
	//
	// This is a G2 stopgap: fine for a test chunk, but G3 must do it GPU-side
	// (or have MeshEmit bake it) rather than round-tripping quads through the
	// CPU, which is exactly what ADR-0006 is trying to stop doing.
	TArray<uint64> RebaseQuadsToRegionLocal(const FVoxelGpuRegionResult& Gpu,
	                                        uint32 BricksX, uint32 BricksY)
	{
		const uint32 InteriorX = BricksX - 2;
		const uint32 InteriorY = BricksY - 2;

		TArray<uint64> Rebased;
		Rebased.Reserve(Gpu.Quads.Num());

		for (int32 MaskIndex = 0; MaskIndex < Gpu.QuadCounts.Num(); ++MaskIndex)
		{
			const uint32 Count = Gpu.QuadCounts[MaskIndex];
			if (Count == 0)
			{
				continue;
			}
			const uint32 Start = Gpu.QuadOffsets[MaskIndex];

			// maskIndex = meshBrickIndex * 48 + axis * 16 + dir * 8 + slice
			const uint32 MeshBrickIndex = uint32(MaskIndex) / 48u;
			const uint32 Ix = MeshBrickIndex % InteriorX;
			const uint32 Iy = (MeshBrickIndex / InteriorX) % InteriorY;
			const uint32 Iz = MeshBrickIndex / (InteriorX * InteriorY);

			// Interior brick (ix,iy,iz) is region brick (ix+1, iy+1, iz+1).
			const uint32 BrickOrigin[3] = { (Ix + 1u) * 8u, (Iy + 1u) * 8u, (Iz + 1u) * 8u };

			for (uint32 Q = 0; Q < Count; ++Q)
			{
				const uint64 Packed = Gpu.Quads[int32(Start + Q)];
				const uint32 W0 = uint32(Packed & 0xffffffffull);
				const uint32 W1 = uint32(Packed >> 32);

				const uint32 Axis  =  W0        & 0xfu;
				const uint32 Dir   = (W0 >>  4) & 0xfu;
				const uint32 Slice = (W0 >>  8) & 0xffu;
				const uint32 U0    = (W0 >> 16) & 0xffu;
				const uint32 V0    = (W0 >> 24) & 0xffu;

				const uint32 U = (Axis + 1u) % 3u;
				const uint32 V = (Axis + 2u) % 3u;

				const uint32 NewSlice = Slice + BrickOrigin[Axis];
				const uint32 NewU0    = U0    + BrickOrigin[U];
				const uint32 NewV0    = V0    + BrickOrigin[V];

				const uint32 NewW0 = Axis | (Dir << 4) | (NewSlice << 8)
				                   | (NewU0 << 16) | (NewV0 << 24);
				Rebased.Add(uint64(NewW0) | (uint64(W1) << 32));
			}
		}
		return Rebased;
	}

	void SpawnTestChunkCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("No world"));
			return;
		}
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("Needs SM6. Relaunch with -sm6."));
			return;
		}

		// Mesh the origin fixture on the GPU -- the same region the digest gate
		// uses, so its contents are already known-good.
		vxc::SyntheticTileSampler Tiles(kSeed);
		vxc::SyntheticTileSampler CpuTiles(kSeed);
		vxc::Amplifier CpuAmp(kSeed, CpuTiles);

		TArray<vxc::ColumnSample> CpuColumns;
		const FVoxelGpuRegionRequest Req = BuildRequest(kRegions[0], CpuAmp, Tiles, CpuColumns);
		const FVoxelGpuRegionResult Gpu = VoxelGpuWorldGen::RunRegionBlocking(Req);

		if (!Gpu.bOk)
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("GPU mesh failed: %s"), *Gpu.Error);
			return;
		}
		if (Gpu.Quads.IsEmpty())
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("GPU produced no quads — nothing to draw"));
			return;
		}

		const TArray<uint64> Rebased = RebaseQuadsToRegionLocal(
			Gpu, kRegions[0].Width / 8, kRegions[0].Height / 8);

		// Positioned from the CAMERA, not the pawn.
		//
		// The pawn's actor forward is not necessarily where the player is
		// looking -- on a fly pawn the view can be rotated independently -- so
		// "pawn location + actor forward" can put the chunk cleanly off-screen,
		// which is indistinguishable from it failing to render. GetPlayerViewPoint
		// is what the player actually sees.
		//
		// Lifted well above the view direction too, so it reads against open sky
		// instead of being buried in the hillside the camera is pointed at.
		FVector SpawnLocation = FVector::ZeroVector;
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector CamLoc = FVector::ZeroVector;
			FRotator CamRot = FRotator::ZeroRotator;
			PC->GetPlayerViewPoint(CamLoc, CamRot);
			SpawnLocation = CamLoc + CamRot.Vector() * 2000.0 + FVector(0, 0, -700);
		}

		// CONTROL EXPERIMENT: "voxel.GPU.SpawnTestChunk control" puts an ordinary
		// engine cube at the SAME place instead of the GPU chunk.
		//
		// Worth its few lines. Everything so far has assumed the test rig is
		// sound -- that the spawn point is in view, lit, and not inside a hill.
		// That assumption has never been checked, and if it is wrong then every
		// "invisible" result so far says nothing about the draw path. A stock
		// static mesh either shows up or it does not, and either answer is
		// worth more than another round of guessing at the renderer.
		const bool bControl = Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("control"), ESearchCase::IgnoreCase); });
		if (bControl)
		{
			AActor* ControlActor = World->SpawnActor<AActor>(
				AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator);
			UStaticMeshComponent* SM = NewObject<UStaticMeshComponent>(ControlActor);
			ControlActor->SetRootComponent(SM);
			// Try both cube paths and SAY which one worked. The first version of
			// this control silently did nothing when the mesh failed to load,
			// which would have made "the control is invisible too" a false
			// negative -- the worst possible outcome for a control experiment.
			UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (Cube == nullptr)
			{
				Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EngineMeshes/Cube.Cube"));
			}
			UE_LOG(LogVoxelGpuVerify, Log, TEXT("CONTROL: cube mesh %s"),
			       Cube ? *Cube->GetPathName() : TEXT("FAILED TO LOAD"));
			if (Cube)
			{
				SM->SetStaticMesh(Cube);
				SM->SetWorldScale3D(FVector(20.0));   // 20 m cube
			}
			SM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			SM->RegisterComponent();
			// MUST come after RegisterComponent. SetRootComponent on a freshly
			// NewObject'd component installs it with an IDENTITY transform, and
			// since the root component is what defines the actor's location,
			// that silently teleports the actor to the world origin -- 8,448 km
			// from the camera in this test.
			SM->SetWorldLocation(SpawnLocation);
			// Second, asset-free control: a persistent debug box. It goes through
			// an entirely different rendering path from static meshes, so if the
			// box appears and the cube does not, the fault is in the mesh/asset
			// side; if NEITHER appears, the location itself is not visible.
			DrawDebugBox(World, SpawnLocation, FVector(1000.0), FColor::Red, true, -1.0f, 0, 50.0f);
			DrawDebugSphere(World, SpawnLocation, 800.0f, 16, FColor::Yellow, true, -1.0f, 0, 40.0f);

			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("CONTROL: stock cube + debug box/sphere at %s. If NONE of them are ")
			       TEXT("visible, the test rig is at fault, not the GPU draw path."),
			       *SpawnLocation.ToString());
		}

		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator);
		if (Actor == nullptr)
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("Failed to spawn actor"));
			return;
		}
		Actor->SetActorLabel(TEXT("VoxelGpuTestChunk"));

		// The real terrain material, so the A/B compares like with like. Without
		// it the chunk falls back to WorldGridMaterial and every difference is
		// swamped by "one is grey".
		UMaterialInterface* TerrainMaterial = Cast<UMaterialInterface>(StaticLoadObject(
			UMaterialInterface::StaticClass(), nullptr,
			TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain")));
		UE_LOG(LogVoxelGpuVerify, Log, TEXT("Terrain material: %s"),
		       TerrainMaterial ? *TerrainMaterial->GetPathName() : TEXT("NOT FOUND (falling back to grey)"));

		UVoxelGpuChunkComponent* Comp = NewObject<UVoxelGpuChunkComponent>(Actor);
		Actor->SetRootComponent(Comp);
		Comp->SetChunkLevel(0);
		Comp->SetChunkMaterial(TerrainMaterial);
		Comp->SetQuads(Rebased);
		Comp->RegisterComponent();
		// See the control above: without this the chunk sits at the world
		// origin regardless of where the actor was spawned.
		Comp->SetWorldLocation(SpawnLocation);

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("Spawned a GPU-drawn chunk at %s: %d quads, %d triangles, drawn with no vertex ")
		       TEXT("or index buffer."),
		       *SpawnLocation.ToString(), Rebased.Num(), Rebased.Num() * 2);

		// THE ACTUAL G2 GATE: the same quads, drawn by the shipping CPU path,
		// placed alongside. Feeding both renderers identical geometry is what
		// makes the comparison mean something -- any difference is the DRAW
		// path, since the mesher input is byte-identical by construction.
		if (Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("ab"), ESearchCase::IgnoreCase); }))
		{
			TArray<FVoxelChunkQuad> CpuQuads;
			CpuQuads.Reserve(Rebased.Num());
			for (const uint64 Packed : Rebased)
			{
				const uint32 W0 = uint32(Packed & 0xffffffffull);
				const uint32 W1 = uint32(Packed >> 32);
				FVoxelChunkQuad Q;
				Q.Axis     = uint8( W0        & 0xfu);
				Q.Positive = uint8((W0 >>  4) & 0xfu);
				Q.Slice    = uint8((W0 >>  8) & 0xffu);
				Q.U0       = uint8((W0 >> 16) & 0xffu);
				Q.V0       = uint8((W0 >> 24) & 0xffu);
				Q.W        = uint8( W1        & 0xffu);
				Q.H        = uint8((W1 >>  8) & 0xffu);
				Q.Ao       = uint8((W1 >> 16) & 0xffu);
				Q.Mat      = uint8((W1 >> 24) & 0xffu);
				CpuQuads.Add(Q);
			}

			// Offset sideways so both are in frame at once. 800 UU = 8 m, a bit
			// wider than the 6.4 m region, so they sit adjacent without overlap.
			const FVector CpuLocation = SpawnLocation + FVector(0, 800, 0);
			AActor* CpuActor = World->SpawnActor<AActor>(
				AActor::StaticClass(), CpuLocation, FRotator::ZeroRotator);
			UVoxelChunkComponent* CpuComp = NewObject<UVoxelChunkComponent>(CpuActor);
			CpuActor->SetRootComponent(CpuComp);
			if (TerrainMaterial)
			{
				CpuComp->SetMaterial(0, TerrainMaterial);
			}
			// 64, not 32: this is a region, not one chunk.
			CpuComp->SetChunkQuads(MoveTemp(CpuQuads), 64);
			CpuComp->RegisterComponent();
			CpuComp->SetWorldLocation(CpuLocation);

			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("A/B: CPU-meshed chunk from the SAME quads at %s (GPU on the left, "
			            "CPU 8 m to its right). They should be indistinguishable."),
			       *CpuLocation.ToString());
		}

		// Optional self-check: "voxel.GPU.SpawnTestChunk shot" grabs a
		// screenshot a couple of seconds later.
		//
		// The delay is the point. The component only marks its render state
		// dirty here; the scene proxy is built, its RHI resources created and
		// the first frame drawn some frames afterwards. Shooting immediately
		// would reliably capture an empty sky and read as a failure.
		//
		// This exists so the visual half of the G2 gate can be inspected from a
		// headless run instead of costing someone a play session.
		const bool bWantScreenshot = Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("shot"), ESearchCase::IgnoreCase); });
		if (bWantScreenshot)
		{
			FTimerHandle Handle;
			TWeakObjectPtr<UWorld> WeakWorld(World);
			const FVector SpawnedAt = SpawnLocation;
			World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakWorld, SpawnedAt]()
			{
				// Log where the camera is NOW, not where it was at spawn time.
				// The control cube being invisible proves the rig is wrong, and
				// the likeliest reason is that the view moves between the
				// command running at startup and the screenshot 3 s later --
				// leaving everything spawned 20 m from where the camera used
				// to be.
				if (UWorld* W = WeakWorld.Get())
				{
					if (APlayerController* PC = W->GetFirstPlayerController())
					{
						FVector CamLoc = FVector::ZeroVector;
						FRotator CamRot = FRotator::ZeroRotator;
						PC->GetPlayerViewPoint(CamLoc, CamRot);
						UE_LOG(LogVoxelGpuVerify, Log,
						       TEXT("AT SCREENSHOT: camera %s rot %s | spawned at %s | distance %.0f UU"),
						       *CamLoc.ToString(), *CamRot.ToString(), *SpawnedAt.ToString(),
						       FVector::Dist(CamLoc, SpawnedAt));
					}
				}
				FScreenshotRequest::RequestScreenshot(false);
				UE_LOG(LogVoxelGpuVerify, Log, TEXT("Screenshot requested (Saved/Screenshots/)"));
			}), 3.0f, false);
		}
	}

	FAutoConsoleCommandWithWorldAndArgs GVoxelGpuSpawnTestChunkCmd(
		TEXT("voxel.GPU.SpawnTestChunk"),
		TEXT("Mesh a region on the GPU and draw it in front of the player through the GPU vertex "
		     "factory (no vertex/index buffer). The G2 visual check."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpawnTestChunkCommand));
}


// ---------------------------------------------------------------------------
// voxel.GPU.SpawnPool <N> — the ADR-0006 shape, demonstrated
//
// Puts N chunks into ONE pool, drawn by ONE primitive in ONE draw call. The
// single-chunk command proves geometry is correct; this proves the thing the
// ADR is actually for: that how many chunks are resident has no bearing on how
// many primitives or draws the renderer sees.
//
// It meshes ONE region on the GPU and places that geometry at N different
// origins. Re-meshing N distinct regions would cost N times the setup and
// prove nothing extra about the DRAW path, which is what is under test here.
// ---------------------------------------------------------------------------

namespace
{
	// Climate for one chunk, sampled at its world centre.
	//
	// The CPU path samples per QUAD; this samples per CHUNK. A chunk is 3.2 m
	// across a 30 m climate raster cell -- roughly 1/100th of a pixel's area --
	// and climate varies as a smooth bilinear ramp over that distance, so the
	// difference is a gentle chunk-to-chunk gradient rather than banding. The
	// CPU path's own comment already describes its per-quad sampling as ~10x
	// oversampling the source raster.
	//
	// Returns 0..1 temperature/precipitation, matching what the CPU path writes
	// into vertex colour B/A for T_VoxelBiomeLUT.
	FVector4f SampleChunkClimate(const FVector& PoolWorldOrigin, const FVector3f& ChunkOriginUU)
	{
		// Chunk centre: the region is 64 voxels, so 320 UU in from its origin.
		const double CentreX = PoolWorldOrigin.X + double(ChunkOriginUU.X) + 320.0;
		const double CentreY = PoolWorldOrigin.Y + double(ChunkOriginUU.Y) + 320.0;

		VoxelClimate::EnsureInitialized();
		const FVoxelClimateBytes Bytes = VoxelClimate::SampleClimateAtWorldUU(CentreX, CentreY);
		// Z is left at "no surface gate" deliberately. This pool is the
		// known-good CONTROL (docs/gpu-pool-rendering-notes.md: "run the
		// known-good test pool in the same process, same frame, same material as
		// the failing one"), so its image has to stay comparable to every earlier
		// screenshot of it. Gating it would change the reference for a reason
		// that has nothing to do with what the control tests.
		return FVector4f(float(Bytes.Temperature) / 255.0f,
		                 float(Bytes.Precipitation) / 255.0f,
		                 UVoxelGpuPoolComponent::kNoSurfaceGate,
		                 0.0f);
	}

	void SpawnPoolCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr || !VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("No world, or SM6 unavailable."));
			return;
		}

		const int32 NumChunks = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 4096) : 64;

		vxc::SyntheticTileSampler Tiles(kSeed);
		vxc::SyntheticTileSampler CpuTiles(kSeed);
		vxc::Amplifier CpuAmp(kSeed, CpuTiles);

		TArray<vxc::ColumnSample> CpuColumns;
		const FVoxelGpuRegionRequest Req = BuildRequest(kRegions[0], CpuAmp, Tiles, CpuColumns);
		const FVoxelGpuRegionResult Gpu = VoxelGpuWorldGen::RunRegionBlocking(Req);
		if (!Gpu.bOk || Gpu.Quads.IsEmpty())
		{
			UE_LOG(LogVoxelGpuVerify, Error, TEXT("GPU mesh failed: %s"), *Gpu.Error);
			return;
		}

		const TArray<uint64> Rebased = RebaseQuadsToRegionLocal(Gpu, kRegions[0].Width / 8, kRegions[0].Height / 8);

		FVector SpawnLocation = FVector::ZeroVector;
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector CamLoc; FRotator CamRot;
			PC->GetPlayerViewPoint(CamLoc, CamRot);
			SpawnLocation = CamLoc + CamRot.Vector() * 3000.0 + FVector(0, 0, -900);
		}

		UMaterialInterface* TerrainMaterial = Cast<UMaterialInterface>(StaticLoadObject(
			UMaterialInterface::StaticClass(), nullptr,
			TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain")));

		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator);
		UVoxelGpuPoolComponent* Pool = NewObject<UVoxelGpuPoolComponent>(Actor);
		Actor->SetRootComponent(Pool);
		Pool->SetChunkMaterial(TerrainMaterial);

		// Capacity with headroom, so churn has somewhere to go.
		Pool->InitPool(uint32(Rebased.Num()) * uint32(NumChunks) * 2u);

		// Lay the chunks out in a grid. 640 UU = 6.4 m is the region edge, so
		// they tile without overlapping.
		const int32 Side = FMath::CeilToInt(FMath::Sqrt(double(NumChunks)));
		TArray<int32> Handles;
		for (int32 I = 0; I < NumChunks; ++I)
		{
			const int32 Gx = I % Side;
			const int32 Gy = I / Side;
			const FVector3f Origin(float(Gx) * 640.0f, float(Gy) * 640.0f, 0.0f);
			Handles.Add(Pool->AddChunk(Rebased, Origin, 0,
				SampleChunkClimate(SpawnLocation, Origin)));
		}

		// CHURN: drop every other chunk and re-add half of them. This is what
		// streaming actually does, and the point is that none of it touches
		// FScene -- the primitive count is 1 before, during and after.
		if (Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("churn"), ESearchCase::IgnoreCase); }))
		{
			int32 Removed = 0;
			for (int32 I = 0; I < Handles.Num(); I += 2)
			{
				if (Handles[I] != INDEX_NONE)
				{
					Pool->RemoveChunk(Handles[I]);
					++Removed;
				}
			}
			int32 ReAdded = 0;
			for (int32 I = 0; I < Handles.Num() / 4; ++I)
			{
				const int32 Gx = I % Side;
				const int32 Gy = I / Side;
				const FVector3f ReAddOrigin(float(Gx) * 640.0f, float(Gy) * 640.0f, 900.0f);
				if (Pool->AddChunk(Rebased, ReAddOrigin, 0,
					SampleChunkClimate(SpawnLocation, ReAddOrigin)) != INDEX_NONE)
				{
					++ReAdded;
				}
			}
			UE_LOG(LogVoxelGpuVerify, Log,
			       TEXT("Churn: removed %d, re-added %d — %d live chunks, %u quads used, "
			            "%u free, largest run %u"),
			       Removed, ReAdded, Pool->GetNumChunks(), Pool->GetHighWaterMarkQuads(),
			       Pool->GetFreeQuads(), Pool->GetLargestFreeRun());
		}

		Pool->RegisterComponent();
		Pool->SetWorldLocation(SpawnLocation);

		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("Pool: %d live chunks, %u quads used, %d triangles in ONE primitive at %s"),
		       Pool->GetNumChunks(), Pool->GetHighWaterMarkQuads(),
		       int32(Pool->GetHighWaterMarkQuads()) * 2, *SpawnLocation.ToString());

		// "churnlive" is the mode that actually tests G3.
		//
		// Plain "churn" adds and churns inside this one console command, so the
		// scene proxy does not exist yet for any of it and every edit collapses
		// into a single full rebuild at end of frame. The incremental upload
		// path -- the thing streaming will lean on constantly -- never runs, and
		// a bug in it renders a perfectly clean screenshot. That is precisely
		// how a wrong buffer usage flag survived verification once already.
		//
		// Deferring the churn by a few seconds puts it after the proxy is live,
		// so RemoveChunk/AddChunk/UpdateChunk go through PushUpdatesToProxy and
		// write real sub-ranges of a real GPU buffer.
		if (Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("churnlive"), ESearchCase::IgnoreCase); }))
		{
			TWeakObjectPtr<UVoxelGpuPoolComponent> WeakPool(Pool);
			FTimerHandle ChurnHandle;
			World->GetTimerManager().SetTimer(ChurnHandle, FTimerDelegate::CreateLambda(
				[WeakPool, Handles, Rebased, Side, SpawnLocation]()
			{
				UVoxelGpuPoolComponent* LivePool = WeakPool.Get();
				if (LivePool == nullptr)
				{
					return;
				}

				int32 Removed = 0;
				for (int32 I = 0; I < Handles.Num(); I += 2)
				{
					if (Handles[I] != INDEX_NONE)
					{
						LivePool->RemoveChunk(Handles[I]);
						++Removed;
					}
				}

				// Re-add into the runs just freed, one Z-step up so the result is
				// visually unambiguous: anything still drawing at the old height
				// is a stale range that was not actually overwritten.
				int32 ReAdded = 0;
				for (int32 I = 0; I < Handles.Num() / 4; ++I)
				{
					const FVector3f ReAddOrigin(
						float(I % Side) * 640.0f, float(I / Side) * 640.0f, 900.0f);
					if (LivePool->AddChunk(Rebased, ReAddOrigin, 0,
						SampleChunkClimate(SpawnLocation, ReAddOrigin)) != INDEX_NONE)
					{
						++ReAdded;
					}
				}

				// And re-mesh a survivor in place, which is the case G3 hits on
				// every dig: same handle, same slot, rewritten contents.
				int32 Updated = 0;
				for (int32 I = 1; I < Handles.Num(); I += 8)
				{
					if (Handles[I] != INDEX_NONE &&
					    LivePool->UpdateChunk(Handles[I], Rebased) != INDEX_NONE)
					{
						++Updated;
					}
				}

				UE_LOG(LogVoxelGpuVerify, Log,
				       TEXT("Live churn (proxy already up): removed %d, re-added %d, "
				            "updated-in-place %d — %d live chunks, %u high water, %u free"),
				       Removed, ReAdded, Updated, LivePool->GetNumChunks(),
				       LivePool->GetHighWaterMarkQuads(), LivePool->GetFreeQuads());
			}), 5.0f, false);
		}

		if (Args.ContainsByPredicate(
			[](const FString& A) { return A.Equals(TEXT("shot"), ESearchCase::IgnoreCase); }))
		{
			FTimerHandle Handle;
			// 10 s, not 3. The pool is spawned into the live streamed world, so
			// a shot taken before the CPU cascade has filled shows half-loaded
			// terrain around the pool and invites blaming the pool for it --
			// which is exactly what happened once.
			World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([]()
			{
				FScreenshotRequest::RequestScreenshot(false);
			}), 10.0f, false);
		}
	}

	// Screenshot the live game N seconds from now.
	//
	// Exists because the interesting question is usually "what does the normal
	// streamed world look like under some cvar", and every screenshot harness
	// before this was welded to a spawn command. Headless runs pair it with
	// -ExecCmds, e.g. "voxel.Stream.GPU 1, voxel.Debug.ShotIn 25" -- long
	// enough for the cascade to fill, since a shot taken mid-fill shows coarse
	// LOD and reads as a rendering bug.
	void ShotInCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}
		const float Delay = (Args.Num() > 0) ? FMath::Max(0.1f, FCString::Atof(*Args[0])) : 20.0f;
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([]()
		{
			FScreenshotRequest::RequestScreenshot(false);
			UE_LOG(LogVoxelGpuVerify, Log, TEXT("Screenshot requested (Saved/Screenshots/)"));
		}), Delay, false);
		UE_LOG(LogVoxelGpuVerify, Log, TEXT("Screenshot scheduled in %.1f s"), Delay);
	}

	FAutoConsoleCommandWithWorldAndArgs GVoxelDebugShotInCmd(
		TEXT("voxel.Debug.ShotIn"),
		TEXT("Take a screenshot N seconds from now (default 20). Usage: voxel.Debug.ShotIn [seconds]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ShotInCommand));

	// Set a cvar N seconds from now, so ONE session can screenshot BOTH sides of
	// an A/B.
	//
	// WHY THIS IS NOT A CONVENIENCE. -ExecCmds runs everything at startup, and
	// every screenshot fixture in this project either fires once and quits
	// (-VoxelScreenshotAfter) or cannot change anything between shots
	// (voxel.Debug.ShotIn). So a visual A/B has meant two sessions -- and Wave A
	// measured this project's screenshot noise floor to be BIMODAL rather than
	// noise-like: captures fall into two clusters, 0.00% different WITHIN a
	// cluster and 1.81% BETWEEN, from some per-session latch (probably eye
	// adaptation).
	//
	// A cross-session pair therefore carries a latch difference on top of the
	// effect, and 1.81% is larger than plenty of real effects. Same session means
	// same cluster, which is what makes "0.00% differing pixels" a statement about
	// the change rather than about which cluster each run happened to land in.
	//
	// Usage: voxel.Debug.SetIn <seconds> <cvar> <value>
	// Note -ExecCmds splits on COMMAS, so this takes space-separated args and no
	// argument here may contain a comma.
	void SetInCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr || Args.Num() < 3)
		{
			UE_LOG(LogVoxelGpuVerify, Warning,
			       TEXT("voxel.Debug.SetIn: need <seconds> <cvar> <value>"));
			return;
		}
		const float Delay = FMath::Max(0.0f, FCString::Atof(*Args[0]));
		const FString CvarName = Args[1];
		const FString Value = Args[2];

		// Resolved NOW, not at fire time, so a typo is a loud error at t=0 rather
		// than a silent no-op forty seconds later in the middle of a measurement.
		// A misspelt cvar that quietly does nothing would leave an A/B running two
		// identical configs and looking exactly like a null result.
		if (IConsoleManager::Get().FindConsoleVariable(*CvarName) == nullptr)
		{
			UE_LOG(LogVoxelGpuVerify, Error,
			       TEXT("voxel.Debug.SetIn: no such cvar '%s' -- NOT scheduling. This would have run an A/B "
			            "against itself."), *CvarName);
			return;
		}

		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([CvarName, Value]()
		{
			if (IConsoleVariable* Cvar = IConsoleManager::Get().FindConsoleVariable(*CvarName))
			{
				Cvar->Set(*Value, ECVF_SetByConsole);
				// Echoed back by READING it, not by repeating what was asked for.
				// A cvar can refuse or clamp a value, and the log has to say what
				// the run actually used or it is not evidence.
				UE_LOG(LogVoxelGpuVerify, Log,
				       TEXT("voxel.Debug.SetIn FIRED: %s = %s (requested %s)"),
				       *CvarName, *Cvar->GetString(), *Value);
			}
		}), Delay, false);
		UE_LOG(LogVoxelGpuVerify, Log,
		       TEXT("voxel.Debug.SetIn scheduled: %s = %s in %.1f s"), *CvarName, *Value, Delay);
	}

	FAutoConsoleCommandWithWorldAndArgs GVoxelDebugSetInCmd(
		TEXT("voxel.Debug.SetIn"),
		TEXT("Set a cvar N seconds from now, so one session can capture both sides of an A/B against a "
		     "within-session screenshot floor. Usage: voxel.Debug.SetIn <seconds> <cvar> <value>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetInCommand));

	FAutoConsoleCommandWithWorldAndArgs GVoxelGpuSpawnPoolCmd(
		TEXT("voxel.GPU.SpawnPool"),
		TEXT("Put N chunks in one GPU pool drawn by ONE primitive in ONE draw call. "
		     "Usage: voxel.GPU.SpawnPool [N] [churn|churnlive] [shot]. "
		     "churn edits before the proxy exists (one rebuild); churnlive edits "
		     "after it is up, which is the path streaming actually uses."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpawnPoolCommand));
}
