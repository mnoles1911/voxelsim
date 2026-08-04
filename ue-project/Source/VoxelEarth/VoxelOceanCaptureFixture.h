#pragma once

#include "CoreMinimal.h"

class UWorld;

// -VoxelOceanDig / -VoxelOceanSurvey: the two switches the ocean captures need
// and that no existing fixture could supply.
//
// WHY THIS EXISTS. Work item 8 replaced Reservoir v0 -- breach-seeded voxels
// pinned to 255 fill units forever -- with `oceanSurfaceMmAt` composed into
// `implicitWaterDatumMm`, and voxel-core/tests/test_ocean.cpp measures the
// three defects that removed. None of that has ever been LOOKED at, and the
// looking is constrained in a way the existing fixtures cannot satisfy:
//
//   * near-field implicit water exists only inside kImplicitRadiusBricks /
//     kImplicitRadiusBricksZ of the camera brick -- +/-25.6 m in xy and
//     +/-12.8 m in z. A dig outside that box is meshed correctly and is
//     invisible, which reads as "the feature does not work" and is not;
//   * -VoxelHeadlessDigTest carves at pawn+100 m, an order of magnitude
//     outside that box in xy;
//   * -VoxelBreachTest scans for its own column around the WORLD ORIGIN (its
//     comment says "around spawn"; the code multiplies the loop index by the
//     step and adds nothing), so the carve can land 20 km from the camera and
//     is unusable for framing;
//   * -VoxelDigDownTest carves one 60 m shaft at pawn+6 m, which is deeper
//     than the z half-box and closer than the near clip of any useful pitch.
//
// So: dig AT A NAMED COLUMN, in the shape the test measures, near enough to a
// deliberately posed camera to be inside the implicit-water box.
//
// THE SITE IS NAMED, NEVER SEARCHED. Three of nine vista sites in this project
// were wrong when they were picked off a map by eye, including a "beach" that
// was a photograph of open water; the fix was to re-derive the labels at the
// exact column through the real classifiers. Same rule here, and it is why
// -VoxelOceanSurvey exists as a mode rather than as a search inside the dig:
// the survey prints `UVoxelWorldSubsystem::GetSurfaceHeightUU` -- the SAME
// amplified ground `oceanSurfaceMmAt` is gated on -- over a grid, so the
// column a capture digs at is chosen from the engine's own answer and stated
// in the log. A fixture that found its own column would move it silently the
// next time the amplifier changed.
//
// AND IT CHECKS THE SITE AGAIN AT DIG TIME, at Error, and refuses to dig: a
// pit whose column is below sea level is not an inland pit, and a breach whose
// column is above it is not a breach. Either would produce a frame that gets
// judged on the wrong thing, which is the failure this whole file is built to
// avoid.
namespace VoxelOceanCaptureFixture
{
// Parses the switch set and arms whichever mode was asked for. Returns true if
// it armed anything. Called from AVoxelEarthGameMode::BeginPlay beside the
// other water fixtures.
//
// Run state lives in a heap object owned by the timer delegates, following
// VoxelSweBreachFixture: it keeps the carried state (a chosen site, a per-pass
// water snapshot table) out of AVoxelEarthGameMode.h, which every other class
// in the module already includes.
//
// NEITHER MODE QUITS THE PROCESS. That is deliberate and is the difference
// from every other fixture here: these runs are driven by
// tools/voxel-capture.ps1, whose whole contract is that -VoxelScreenshotAfter
// owns the shutter and the exit. A self-quit would race it.
bool StartFromCommandLine(UWorld* World);
} // namespace VoxelOceanCaptureFixture
