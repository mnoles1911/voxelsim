#pragma once

#include "CoreMinimal.h"

class UWorld;

// -VoxelSweBreachTest: the fixture that DISCRIMINATES the two readings of the
// W4 shallow-water layer's behaviour (docs/adr/0004-swe-fixed-point-coupling.md).
//
// THE QUESTION IT EXISTS TO SETTLE. With voxel.Water.SWE on, a 30,000-unit pour
// onto near-flat ground spreads as a thin film; with it off, the CA settles the
// same volume into a compact basin. Volume is conserved exactly either way. The
// first-pass reading is "the CA looks more correct, tune SWE toward pooling",
// and that reading may well be right -- but a GENTLE POUR ONTO NEAR-FLAT GROUND
// CANNOT TELL IT APART FROM ITS OPPOSITE:
//
//   (a) shallow water correctly spreading as a thin film, which is the entire
//       thing SWE is for and the thing the CA structurally cannot do
//       (waterca.h Phase C computes "a static equilibrium level ... never a
//       surge"); versus
//   (b) beds seated slightly wrong, so the sheet rests where it should have
//       drained, and the water sits at the wrong height.
//
// Both of those look like a film. Damping/absorption tuned toward pooling
// before that ambiguity is resolved would BURY (b) permanently -- the symptom
// would be gone and the cause would still be there, now with a tuning constant
// on top of it to confuse the next attribution.
//
// WHAT DISCRIMINATES THEM is a breach or a real slope. Correct SWE produces a
// visible SURGE: a directed velocity front at the breach mouth that arrives
// downstream a measurable number of ticks later, and then decays. Wrong beds
// produce water sitting on a hillside, or settling to a level that does not
// match the basin's own geometry. Those two are different SIGNATURES, not
// different screenshots -- which is why this fixture logs velocityAt() and
// settled surface height per column per tick rather than taking a picture and
// asking someone to squint at it.
//
// So the fixture: finds real relief, carves a basin into it, measures the
// basin's own spill level and capacity geometrically, pours a computed volume,
// AUDITS EVERY SEATED BED against the live terrain, breaches the downhill rim
// with UVoxelWorldSubsystem::CarveSphere (the edit-log authority path digging
// uses), and then samples the coupled state at the fixed-step cadence for
// several seconds. Every number it prints is defined in the log line that
// prints it, so the output interprets itself.
//
// It ALWAYS self-quits, including on every failure path and behind an absolute
// watchdog, because the -VoxelWaterParityTest fixture once did not and left an
// editor running for 2h40m.
namespace VoxelSweBreachFixture
{
// Parses the switch set off the command line and schedules the run if
// -VoxelSweBreachTest[=<delaySeconds>] is present. Returns true if it armed.
// Called from AVoxelEarthGameMode::BeginPlay beside the other water fixtures.
//
// All run state lives in a heap object owned by the timer delegates rather
// than in AVoxelEarthGameMode member handles: this sequence has a dozen
// stages and a lot of carried state (a surveyed heightfield, a flooded region,
// a probe set), and threading that through a dozen member FTimerHandles the
// way the older fixtures do would put several hundred lines of one test into a
// header shared by everything else.
bool StartFromCommandLine(UWorld* World);
} // namespace VoxelSweBreachFixture
