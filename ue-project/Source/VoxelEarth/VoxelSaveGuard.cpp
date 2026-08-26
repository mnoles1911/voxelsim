#include "VoxelSaveGuard.h"

#include "VoxelDebug.h" // LogVoxelEdit -- the persistence category

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#include "voxelcore/core.h"    // kWorldGenVersion
#include "voxelcore/editlog.h" // EditLog::peekHeader + the format constants
#include "voxelcore/waterca.h" // WaterState::peekHeader, kWaterCAVersion
#include "voxelcore/bytes.h"

namespace VoxelSaveGuardDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// PROCESS-WIDE, not per-subsystem, and deliberately so. The terrain log, the
// water blob and the hydrology blob are written by three different objects
// with three different lifetimes; a latch owned by any one of them would not
// be visible to the other two, and the whole point is that the class is closed
// once rather than three times.
FCriticalSection GLock;
TMap<FString, FString> GQuarantined; // normalized path -> the first reason

// Loads and saves both run on the game thread today, so the lock is not
// carrying live contention -- it is here because a background save is an
// obvious future addition and a data race on THIS map would re-open exactly
// the hazard the map exists to close.

// One spelling per file, so `Saved/VoxelWorlds/1.vxlog` latched by the loader
// is recognised by a writer handed the absolute path (and vice versa) -- on a
// case-insensitive filesystem, in either case.
FString NormalizePath(const FString& Path)
{
	FString Full = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Full);
	return Full.ToLower();
}
} // namespace VoxelSaveGuardDetail

namespace VoxelSaveGuard
{
// --- classification --------------------------------------------------------

FRefusal ClassifyEditLog(const uint8* Data, const int32 NumBytes)
{
	const size_t Size = size_t(FMath::Max(NumBytes, 0));
	const vxc::EditLog::HeaderPeek H = vxc::EditLog::peekHeader(Data, Size);

	FRefusal Out;
	if (!H.haveMagic || !H.haveFormat || !H.haveWorldGen)
	{
		Out.Token = TEXT("truncated");
		Out.Detail = FString::Printf(
			TEXT("only %d byte(s) on disk -- too short to even hold the edit-log header (magic, format, worldgen). "
			     "This is a truncated or empty file, not a version mismatch."),
			NumBytes);
		return Out;
	}
	if (H.magic != vxc::EditLog::kMagic)
	{
		Out.Token = TEXT("not-a-vxlog");
		Out.Detail = FString::Printf(
			TEXT("magic is 0x%08X, expected 0x%08X (\"VXEL\") -- this is not an edit log at all."),
			H.magic, vxc::EditLog::kMagic);
		return Out;
	}
	if (H.format < vxc::EditLog::kMinReadableFormatVersion || H.format > vxc::EditLog::kFormatVersion)
	{
		Out.bVersionMismatch = true;
		Out.Token = FString::Printf(TEXT("fmt%u"), H.format);
		Out.Detail = FString::Printf(
			TEXT("edit-log FORMAT version %u; this build reads %u..%u. The bytes are intact -- this is a format "
			     "version this build does not speak, not damage."),
			H.format, vxc::EditLog::kMinReadableFormatVersion, vxc::EditLog::kFormatVersion);
		return Out;
	}
	if (H.worldGen != vxc::kWorldGenVersion)
	{
		Out.bVersionMismatch = true;
		Out.Token = FString::Printf(TEXT("wgen%u"), H.worldGen);
		Out.Detail = FString::Printf(
			TEXT("recorded at kWorldGenVersion %u; this build generates %u. THE BYTES ARE INTACT -- this is a "
			     "WORLDGEN BUMP, not corruption. The edits are diffs against terrain this build no longer "
			     "produces, which is why they are refused rather than replayed; a migration written later can "
			     "still read every one of them."),
			H.worldGen, vxc::kWorldGenVersion);
		return Out;
	}

	// Header agrees on every version field, so whatever went wrong is in the
	// body: a truncated entry, a non-contiguous seq, an out-of-range cell, or
	// trailing bytes. Genuinely damaged, and worth saying so plainly, because
	// this is the ONE case where "corrupt" was the right word all along.
	Out.Token = TEXT("damaged");
	Out.Detail = FString::Printf(
		TEXT("the header is VALID (format %u, kWorldGenVersion %u, seed %llu, brickEdge %u) and matches this "
		     "build, so the damage is in the entry stream: truncation, a bad entry, or trailing bytes. %d byte(s) "
		     "on disk. This is NOT a version mismatch."),
		H.format, H.worldGen, (unsigned long long)H.seed, uint32(H.brickEdge), NumBytes);
	return Out;
}

FRefusal ClassifyWaterState(const uint8* Data, const int32 NumBytes)
{
	const size_t Size = size_t(FMath::Max(NumBytes, 0));
	const vxc::WaterState::HeaderPeek H = vxc::WaterState::peekHeader(Data, Size);

	FRefusal Out;
	if (!H.haveMagic || !H.haveFormat || !H.haveCaVersion)
	{
		Out.Token = TEXT("truncated");
		Out.Detail = FString::Printf(
			TEXT("only %d byte(s) on disk -- too short to hold the water-state header (magic, format, CA version)."),
			NumBytes);
		return Out;
	}
	if (H.magic != vxc::WaterState::kMagic)
	{
		Out.Token = TEXT("not-a-vxwater");
		Out.Detail = FString::Printf(
			TEXT("magic is 0x%08X, expected 0x%08X (\"VXWA\") -- this is not a water-state blob."),
			H.magic, vxc::WaterState::kMagic);
		return Out;
	}
	if (H.format != vxc::WaterState::kFormatVersion)
	{
		Out.bVersionMismatch = true;
		Out.Token = FString::Printf(TEXT("fmt%u"), H.format);
		Out.Detail = FString::Printf(
			TEXT("water-state FORMAT version %u; this build writes and reads %u exactly. The bytes are intact."),
			H.format, vxc::WaterState::kFormatVersion);
		return Out;
	}
	if (H.caVersion != vxc::kWaterCAVersion)
	{
		Out.bVersionMismatch = true;
		Out.Token = FString::Printf(TEXT("caver%u"), H.caVersion);
		Out.Detail = FString::Printf(
			TEXT("recorded under kWaterCAVersion %u; this build ticks v%u. THE BYTES ARE INTACT -- the fill was "
			     "produced by different tick rules, which is exactly what that constant exists to signal."),
			H.caVersion, vxc::kWaterCAVersion);
		return Out;
	}

	Out.Token = TEXT("damaged");
	Out.Detail = FString::Printf(
		TEXT("the header is VALID (format %u, kWaterCAVersion %u) and matches this build, so the refusal came from "
		     "the body: truncation, a non-ascending key, a payload that does not cover 512 cells, or a totalVolume "
		     "that disagrees with the fills. %d byte(s) on disk. This is NOT a version mismatch."),
		H.format, H.caVersion, NumBytes);
	return Out;
}

FRefusal ClassifyContainer(const uint8* Data, const int32 NumBytes, const uint32 ExpectedMagic,
                           const uint32 ExpectedFormatVersion, const TCHAR* Label)
{
	FRefusal Out;
	uint32 Magic = 0;
	uint32 Format = 0;
	bool bHaveMagic = false;
	bool bHaveFormat = false;
	if (Data != nullptr && NumBytes > 0)
	{
		vxc::ByteReader R(Data, size_t(NumBytes));
		uint32_t M = 0;
		uint32_t F = 0;
		bHaveMagic = R.u32(M);
		Magic = M;
		if (bHaveMagic)
		{
			bHaveFormat = R.u32(F);
			Format = F;
		}
	}

	if (!bHaveMagic || !bHaveFormat)
	{
		Out.Token = TEXT("truncated");
		Out.Detail = FString::Printf(TEXT("only %d byte(s) on disk -- too short to hold the %s header."), NumBytes,
		                             Label);
		return Out;
	}
	if (Magic != ExpectedMagic)
	{
		Out.Token = TEXT("wrong-magic");
		Out.Detail = FString::Printf(TEXT("magic is 0x%08X, expected 0x%08X -- this is not a %s."), Magic,
		                             ExpectedMagic, Label);
		return Out;
	}
	if (Format != ExpectedFormatVersion)
	{
		Out.bVersionMismatch = true;
		Out.Token = FString::Printf(TEXT("fmt%u"), Format);
		Out.Detail = FString::Printf(
			TEXT("%s FORMAT version %u; this build reads %u. The bytes are intact -- a version this build does not "
			     "speak, not damage."),
			Label, Format, ExpectedFormatVersion);
		return Out;
	}

	Out.Token = TEXT("damaged");
	Out.Detail = FString::Printf(
		TEXT("the %s header is VALID (format %u) and matches this build, so the refusal came from the body. %d "
		     "byte(s) on disk. This is NOT a version mismatch."),
		Label, Format, NumBytes);
	return Out;
}

// --- the latch -------------------------------------------------------------

void Quarantine(const FString& Path, const FString& Reason)
{
	{
		FScopeLock Lock(&VoxelSaveGuardDetail::GLock);
		const FString Key = VoxelSaveGuardDetail::NormalizePath(Path);
		if (VoxelSaveGuardDetail::GQuarantined.Contains(Key))
		{
			// Already latched. Keep the FIRST reason: it is the one that
			// explains how this session ended up running on an empty world.
			return;
		}
		VoxelSaveGuardDetail::GQuarantined.Add(Key, Reason);
	}

	UE_LOG(LogVoxelEdit, Error,
	       TEXT("SaveGuard: AUTOSAVE DISABLED for %s for the rest of this session. Reason: %s Nothing in this "
	            "process will write that path again -- the file on disk is the player's, and this session is "
	            "running on a world it could not read."),
	       *Path, *Reason);
}

bool IsQuarantined(const FString& Path)
{
	FScopeLock Lock(&VoxelSaveGuardDetail::GLock);
	return VoxelSaveGuardDetail::GQuarantined.Contains(VoxelSaveGuardDetail::NormalizePath(Path));
}

bool RefuseWrite(const FString& Path, const TCHAR* Writer)
{
	FString Reason;
	{
		FScopeLock Lock(&VoxelSaveGuardDetail::GLock);
		const FString* Found = VoxelSaveGuardDetail::GQuarantined.Find(VoxelSaveGuardDetail::NormalizePath(Path));
		if (Found == nullptr)
		{
			return false;
		}
		Reason = *Found;
	}

	UE_LOG(LogVoxelEdit, Error,
	       TEXT("%s: REFUSING to write %s -- this session could not READ that file and writing it now would "
	            "destroy it. Reason it was refused: %s"),
	       Writer, *Path, *Reason);
	return true;
}

FString PreserveAside(const FString& Path, const FString& Token)
{
	IFileManager& Files = IFileManager::Get();
	if (!Files.FileExists(*Path))
	{
		// Nothing to preserve. Reachable only if something removed the file
		// between the loader's existence test and here.
		return FString();
	}

	// STABLE SUFFIX, DERIVED FROM THE FILE'S OWN BYTES. A wall-clock stamp
	// would make every launch of a version-bumped build deposit another copy
	// of the same world -- the player's Saved/ directory fills up with
	// `world.vxlog.rejected-2026-08-26-14-03-11` and there is no way to tell
	// which one is the original. A token like "wgen28" is the same for the
	// same file every time, so the second refusal is a no-op instead.
	const FString Target = Path + TEXT(".rejected-") + (Token.IsEmpty() ? FString(TEXT("unreadable")) : Token);

	if (Files.FileExists(*Target))
	{
		// REPEAT REFUSAL. The copy already aside is the OLDER one and
		// therefore the one worth keeping; overwriting it with a newer refusal
		// of the same shape would be the very data loss this file exists to
		// stop. Leave both where they are -- the live path is quarantined, so
		// nothing can overwrite it either.
		UE_LOG(LogVoxelEdit, Warning,
		       TEXT("SaveGuard: %s already exists, so this file has been refused before. KEEPING the earlier "
		            "preserved copy untouched and leaving %s exactly where it is (it is quarantined, so nothing "
		            "will overwrite it)."),
		       *Target, *Path);
		return Target;
	}

	if (!Files.Move(*Target, *Path, /*Replace*/ false))
	{
		UE_LOG(LogVoxelEdit, Error,
		       TEXT("SaveGuard: could NOT move %s aside to %s. The file stays where it is and stays QUARANTINED, "
		            "so it is still safe from this session -- but a later build looking for a rejected copy will "
		            "not find one at that name."),
		       *Path, *Target);
		return FString();
	}

	UE_LOG(LogVoxelEdit, Warning,
	       TEXT("SaveGuard: preserved %s as %s. The player's world is in that file; nothing has been deleted."),
	       *Path, *Target);
	return Target;
}

void RefuseFile(const FString& Path, const FRefusal& Refusal, const TCHAR* What)
{
	UE_LOG(LogVoxelEdit, Error, TEXT("SaveGuard: REFUSED the %s at %s -- %s"), What, *Path, *Refusal.Detail);

	// ORDER MATTERS: latch BEFORE the move. If the move throws the process off
	// a cliff (a locked file, a full disk), the latch is already armed and the
	// file is already safe. The reverse order leaves a window in which a
	// crash-and-restart could autosave over it.
	Quarantine(Path, Refusal.Detail);
	PreserveAside(Path, Refusal.Token);
}

int32 NumQuarantined()
{
	FScopeLock Lock(&VoxelSaveGuardDetail::GLock);
	return VoxelSaveGuardDetail::GQuarantined.Num();
}
} // namespace VoxelSaveGuard

namespace VoxelSaveGuardCommands
{
// A LATCH NOBODY CAN READ IS A LATCH NOBODY CAN TEST. Every claim this file
// makes -- "the refusal armed the guard", "the autosave was refused" -- is
// otherwise only visible as the ABSENCE of a line in a log, and an absence
// cannot come out the other way. This prints the guard's state on demand, so a
// leg can assert it rather than assume it: quarantine a file, run this, see the
// path and the reason, or see "nothing quarantined" and know the arm did not
// engage.
FAutoConsoleCommand GSaveGuardStatus(
	TEXT("voxel.SaveGuard.Status"),
	TEXT("List every save path this session has QUARANTINED (refused to read, therefore refused to write), with "
	     "the reason each was refused. Empty is the normal answer."),
	FConsoleCommandDelegate::CreateStatic(
		[]()
		{
			const int32 Count = VoxelSaveGuard::NumQuarantined();
			if (Count == 0)
			{
				UE_LOG(LogVoxelEdit, Display,
				       TEXT("voxel.SaveGuard.Status: nothing quarantined -- every save path this session touched "
				            "read back cleanly, and autosave is enabled everywhere."));
				return;
			}
			UE_LOG(LogVoxelEdit, Display,
			       TEXT("voxel.SaveGuard.Status: %d path(s) QUARANTINED. Autosave is disabled for each; the file "
			            "on disk belongs to the player and this session will not write it:"),
			       Count);
			FScopeLock Lock(&VoxelSaveGuardDetail::GLock);
			for (const TPair<FString, FString>& Entry : VoxelSaveGuardDetail::GQuarantined)
			{
				UE_LOG(LogVoxelEdit, Display, TEXT("  %s\n      %s"), *Entry.Key, *Entry.Value);
			}
		}));
} // namespace VoxelSaveGuardCommands
