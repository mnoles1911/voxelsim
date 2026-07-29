#pragma once

#include "CoreMinimal.h"

class UWorld;

// -VoxelSkyLadder=<N>: the ACCEPTANCE ARTIFACT for the W3-W5 day/night cycle.
// N screenshots spread evenly across one simulated day, in ONE process.
//
// WHAT IT IS FOR. The sky work (UVoxelSkySubsystem: the clock, the ephemeris,
// the three-actor light rig, the exposure curve) is not verifiable one frame at
// a time. "Is night dark?" and "does the sun redden at dawn?" are questions
// about a SEQUENCE -- they are answered by looking at the whole cycle in frames
// that differ ONLY in the time of day. That is what this fixture produces: a
// numbered ladder of captures whose filenames carry the hour they are of, from
// a camera that has not moved, with a log line per rung stating the sun altitude
// and azimuth the frame was ACTUALLY rendered at.
//
// It is also the leg that CALIBRATES the exposure curve. VoxelSkySubsystem.cpp's
// ExposureBiasForSunAltitude says in as many words that its anchors are
// "UNMEASURED FIRST GUESSES ... what the W6 capture ladder exists to calibrate".
// Every rung therefore logs the bias the curve resolved to alongside the sun
// altitude that produced it, so the numbers can be corrected against the frames.
//
// ===========================================================================
// WHY ONE PROCESS IS NON-NEGOTIABLE
// ===========================================================================
//
// The obvious implementation is a shell loop: N launches, each with
// -VoxelTimeOfDay=<hh:mm> and -VoxelScreenshotAfter. That produces an
// UNREADABLE ladder, and the reason is measured rather than suspected.
//
// Wave A measured this project's screenshot noise floor and found it BIMODAL
// (VoxelGpuVerify.cpp:2074-2084): captures fall into two clusters, 0.00%
// differing pixels WITHIN a session and 1.81% BETWEEN sessions, from some
// per-session latch -- probably eye adaptation. A cross-session pair therefore
// carries a latch difference on top of whatever it is meant to show, and 1.81%
// is larger than plenty of real effects.
//
// A day/night ladder is precisely a sequence of small brightness differences.
// Spread across N processes, every rung sits in a randomly chosen cluster and
// the per-session latch is added to the very signal being read. Same session
// means same cluster, which is what makes a difference between two rungs a
// statement about the TIME OF DAY rather than about which cluster each launch
// happened to land in. So: one process, N captures, a clock that is SET rather
// than waited for.
//
// ===========================================================================
// WHY THE CLOCK IS SET ABSOLUTELY AND THE TIME SCALE IS PINNED TO ZERO
// ===========================================================================
//
// voxel.Sky.TimeScale is forced to 0 at arm time and every rung sets the world
// epoch to an absolute value. Not "let the clock run and shoot every 150 s":
//   - a running clock DRIFTS between the settle wait and the shutter, so the
//     frame is not of the hour its filename claims (the TimeScale cvar's own
//     help string states this: "the sun must not drift between the settle wait
//     and the shutter");
//   - drift is a function of frame time, so the same leg on a slower machine
//     photographs different hours;
//   - and a wait cannot reach a chosen hour at all without first waiting up to
//     a whole day for it to come round.
// A frozen clock plus an absolute set makes rung i a pure function of i and N.
//
// ===========================================================================
// STRUCTURE: COPIED DELIBERATELY FROM VoxelSweBreachFixture
// ===========================================================================
//
// That fixture is the only multi-capture, state-carrying fixture in the tree and
// it was built as a reusable template. This one reuses all four of its load-
// bearing decisions rather than inventing a second shape for the same problem:
//
//  1. RUN STATE ON THE HEAP, owned by the timer delegates
//     (MakeShared<FSkyLadderRun, ESPMode::ThreadSafe>() captured by value), NOT
//     a member FTimerHandle per stage on AVoxelEarthGameMode. A ladder carries a
//     step index, a latched base epoch, a latched camera pose and a per-rung
//     result table across a stage count that is chosen at RUNTIME by N -- there
//     is no fixed number of handles to declare. See VoxelSweBreachFixture.h:54-60
//     for the same argument, and note the extra force it has here: the older
//     member-handle pattern cannot express "N stages" at all.
//
//  2. ONE ENTRY POINT, bool StartFromCommandLine(UWorld*), returning "did it
//     arm". Armed from AVoxelEarthGameMode::BeginPlay beside the SWE breach
//     fixture, so that the file anyone looking for a fixture opens still lists
//     every fixture -- while none of the body lands in a header that every other
//     class in the module includes.
//
//  3. THE WATCHDOG IS ARMED FIRST, before anything that can stall
//     (VoxelSweBreachFixture.cpp:1638-1641).
//
//  4. ONE TERMINAL PATH, Finish(Run, Reason), which every exit goes through --
//     including the watchdog, including every "subsystem missing" bail. It sets
//     bFinishing, logs, clears every timer handle so no stage can re-enter after
//     the final line, then arms a ~5 s RequestExit. IT ALWAYS SELF-QUITS: the
//     -VoxelWaterParityTest fixture once shipped without one and left an editor
//     running for 2h40m (VoxelSweBreachFixture.h:45-48).
//
// ===========================================================================
// USAGE
// ===========================================================================
//
//   -VoxelSkyLadder=<N>              N rungs across one simulated day. Bare
//                                    -VoxelSkyLadder defaults to 8 (every three
//                                    hours; hits both midnight and noon).
//   -VoxelSkyLadderSettle=<seconds>  Settle before EACH capture. Default 4.0.
//                                    The SkyLight uses real-time capture, so the
//                                    ambient term needs frames to reconverge
//                                    after the sun jumps; too short and the early
//                                    rungs photograph the PREVIOUS rung's sky.
//   -VoxelSkyLadderStartHour=<f>     Hour the ladder starts at, 0..24. Default 0
//                                    (midnight). Rung i is at
//                                    StartHour + i * 24/N, wrapped.
//   -VoxelSkyLadderPreflight=<s>     Fixed wait before the streaming poll starts.
//                                    Default 20.
//   -VoxelDate=MM-DD                 REUSED, not re-implemented: the date is
//                                    UVoxelSkySubsystem's own switch and it has
//                                    already resolved it by the time this fixture
//                                    arms. The ladder walks one game day and
//                                    logs which one it landed on.
//
// Captures land in ue-project/Saved/Screenshots/WindowsEditor/ named
// VoxelSkyLadder_<ii>_<hh>h<mm>, so they sort in ladder order and each frame
// states the hour it is of.
namespace VoxelSkyLadderFixture
{
// Parses the switch set and arms the ladder if -VoxelSkyLadder[=<N>] is present.
// Returns true if it armed. Called from AVoxelEarthGameMode::BeginPlay.
//
// REFUSES TO ARM, loudly, in three cases where the ladder would silently produce
// mislabelled frames rather than no frames -- see the .cpp for each. A leg that
// yields nothing and says why costs one command-line edit; a leg that yields
// eight plausible frames of the wrong hours costs an afternoon of believing them.
bool StartFromCommandLine(UWorld* World);
} // namespace VoxelSkyLadderFixture
