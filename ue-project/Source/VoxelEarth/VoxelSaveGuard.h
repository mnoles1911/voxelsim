#pragma once
// THE REFUSAL GUARD: a save file this build could not read must not be a save
// file this build is allowed to WRITE.
//
// ---------------------------------------------------------------------------
// THE BUG THIS EXISTS TO CLOSE, AS A CLASS
// ---------------------------------------------------------------------------
// Every persistence path in this project has the same three-step shape:
//
//   1. LOAD parses a file. On any refusal -- a version mismatch, a truncation,
//      a failed integrity check -- it logs a line and RETURNS, leaving the
//      session running on a fresh, empty world.
//   2. Nothing records that the refusal happened.
//   3. SHUTDOWN autosaves that fresh, empty world OVER THE SAME PATH.
//
// The player's world is then gone, and the only trace is one log line that
// scrolled past. It has fired here twice already through different doors --
// see UVoxelWorldSubsystem::Deinitialize's comment about "the exact failure
// bWorldBegunPlay was introduced to prevent, arriving through a second door".
// A `kWorldGenVersion` bump is the third door: `EditLog::parse` refuses on
// `wgen != kWorldGenVersion` (voxelcore/editlog.h), so bumping that one
// constant destroys every save on every machine, at the next shutdown, in
// silence.
//
// The two existing locks (`bWorldBegunPlay`, `bWorldSessionStartAttempted`)
// both answer "was this a real session?". Neither can answer "did we fail to
// read what was already there?", which is the question all three doors
// actually turn on. This file answers that one, and it answers it for EVERY
// file rather than for the door we happened to find.
//
// ---------------------------------------------------------------------------
// HOW IT WORKS
// ---------------------------------------------------------------------------
// A process-wide set of QUARANTINED paths. A load path that refuses a file
// that EXISTS latches its path here; every writer checks the latch before it
// opens a temp file and refuses, loudly, naming the path. That check lives in
// the atomic-write helpers (`WriteBytesAtomic`, `WriteWaterBytesAtomic`) rather
// than at each call site, so a writer added later inherits the protection
// instead of having to remember it.
//
// ABSENCE IS NOT REFUSAL, and the distinction is structural rather than
// conventional: every caller tests `FPaths::FileExists` FIRST and returns on
// the absent branch before any parse can run. Quarantine is therefore only
// ever reachable on a path already proven to exist on disk. A genuinely new
// world -- no file, NEW GAME, a headless leg in a clean Saved/ -- never
// touches this code, and saves exactly as it always has.
//
// PRESERVATION. Alongside the latch, a refused file is moved aside to
// `<path>.rejected-<token>`. The token comes from the file's OWN BYTES (the
// version it records, or how it is malformed), so it is STABLE: refusing the
// same file twice produces the same name rather than a growing pile of
// timestamped copies. And an existing preserved copy is NEVER overwritten --
// the first one aside is the oldest and therefore the precious one. If the
// move cannot happen at all, nothing is lost anyway: the latch is what
// protects the file, the rename only makes it findable.
//
// LEGIBILITY. `FRefusal` carries expected-versus-found, so the log says
// "written at kWorldGenVersion 28, this build is 31 -- a version bump, the
// bytes are intact" instead of "corrupt or unrecognized". Those two are
// indistinguishable today and want opposite responses.

#include "CoreMinimal.h"

namespace VoxelSaveGuard
{
// Why a file was refused, in the vocabulary the log line and the preserved
// filename share.
struct VOXELEARTH_API FRefusal
{
	// Filename-safe and STABLE for the same bytes: "wgen28", "fmt3", "caver4",
	// "damaged", "truncated", "not-a-vxlog", "seed-mismatch". Never a clock
	// reading -- see the header comment on repeat refusals.
	FString Token;
	// One sentence for a human: what this build expected, what the file held.
	FString Detail;
	// True when the file is INTACT and merely older/newer than this build.
	// This is the flag a future migration keys off, and the one thing the
	// current log cannot say at all.
	bool bVersionMismatch = false;
};

// --- classification (pure; reads only a fixed-size prefix) -----------------

// A `.vxlog` that `vxc::EditLog::parse` returned std::nullopt for.
VOXELEARTH_API FRefusal ClassifyEditLog(const uint8* Data, int32 NumBytes);

// A `.vxwater` that `vxc::WaterState::load` returned false for.
VOXELEARTH_API FRefusal ClassifyWaterState(const uint8* Data, int32 NumBytes);

// Any of this project's other little magic+version containers (`.vxhydro`).
// `Label` names the format in the message ("hydrology blob").
VOXELEARTH_API FRefusal ClassifyContainer(const uint8* Data, int32 NumBytes, uint32 ExpectedMagic,
                                          uint32 ExpectedFormatVersion, const TCHAR* Label);

// --- the latch -------------------------------------------------------------

// Latch `Path` as unwritable for the rest of the process and say so at Error.
// Idempotent: the FIRST reason is kept, because it is the one that explains
// how the session got here.
//
// CALL THIS ONLY FOR A FILE THAT EXISTS. Quarantining a path that is merely
// absent would disable saving for a brand-new world, which is the opposite of
// the point.
VOXELEARTH_API void Quarantine(const FString& Path, const FString& Reason);

VOXELEARTH_API bool IsQuarantined(const FString& Path);

// Writer-side gate. True means "do not write, I have already logged why".
// `Writer` is the name that appears in the line, e.g. TEXT("SaveWorld").
VOXELEARTH_API bool RefuseWrite(const FString& Path, const TCHAR* Writer);

// Move `Path` aside to `<Path>.rejected-<Token>` without ever overwriting an
// existing preserved copy. Returns where the bytes now live, or an empty
// string if they were left in place (which is safe: the latch, not the
// rename, is what stops them being clobbered).
VOXELEARTH_API FString PreserveAside(const FString& Path, const FString& Token);

// Quarantine + preserve + one Error line carrying `Refusal.Detail`. The single
// call every refusal branch should make. `What` names the file for a human
// ("saved world", "water state").
VOXELEARTH_API void RefuseFile(const FString& Path, const FRefusal& Refusal, const TCHAR* What);

// How many paths are latched. Exists so a test or a leg can assert that a
// refusal ACTUALLY armed the guard rather than trusting that it did -- a
// confirmation that cannot come out the other way is not one. The console
// command `voxel.SaveGuard.Status` prints the same state with reasons.
VOXELEARTH_API int32 NumQuarantined();
} // namespace VoxelSaveGuard
