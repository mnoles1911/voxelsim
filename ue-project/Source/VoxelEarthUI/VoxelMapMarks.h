#pragma once
// The player's named places on the map, on disk, keyed to the world they were
// placed in.
//
// WHY A FILE OF ITS OWN RATHER THAN THE CHECKPOINT SYSTEM.
// docs/session-persistence-plan-2026-09-07.md is explicit that a checkpoint is
// a transactional, all-domain object: a manifest listing every required
// payload, an atomic `current` commit, and a rule that no required file may be
// inferred from its absence. Adding marks to it means a new required payload,
// a manifest schema bump, a migration for every save already on disk, and a
// reader that has to decide what a checkpoint missing its marks payload means.
// That is a session-persistence change, not a UI one, and it would put a
// cosmetic list of place names on the critical path of "can this world load at
// all" -- which the same plan explicitly rules out for presentation state
// ("Cosmetic particles, camera settings, UI, network channels and presentation
// caches are not checkpoint authority").
//
// WHAT IS KEYED TO WHAT. The project already has a per-world, seed-keyed store
// that is NOT the checkpoint system: Saved/VoxelWorlds/<seed>.vxlog is the
// shutdown edit log and Saved/VoxelWorlds/<seed>.vxhydro is the water state
// (UVoxelWaterSubsystem::SaveWaterStateToDisk). Both are named by the decimal
// seed and nothing else. Marks join them as Saved/VoxelWorlds/<seed>.vxmarks.json.
//
// AND THE SEED REALLY IS THE WORLD IDENTITY TODAY. VoxelSaveLibrary.h says so
// in as many words: UVoxelWorldSubsystem resolves Seed once in Initialize,
// before Impl exists, so a save recorded under a different seed "is listed but
// not loadable". Every named save in a session therefore shares one seed, and
// a marks file keyed by seed is found by every future session that can open
// the world at all. The day the session plan's {WorldId, SlotId, GenerationId}
// lands and seeds stop being identity, this file moves with .vxhydro and the
// two are one migration, not two.
//
// TOLERANCE IS THE POINT. A missing file is the ordinary first-run answer. A
// truncated or hand-edited one must cost the player their marks and nothing
// else: no failed load, no refused save, no crash. Load() therefore returns
// whatever it could read and logs what it could not, and every field has a
// defined value when absent.

#include "CoreMinimal.h"
#include "VoxelScreenData.h" // FVoxelMapMark

namespace VoxelMapMarks
{
// The file's own schema version, written into every file and checked on read.
// 1 is the first. A file claiming a HIGHER version is left alone and read as
// empty, so a newer build's marks are never silently rewritten in an older
// build's format.
inline constexpr int32 kFormatVersion = 1;

// Belt-and-braces caps. Neither is a design limit; both exist so a corrupt
// file cannot make the loader allocate without bound.
inline constexpr int32 kMaxMarks = 4096;
inline constexpr int32 kMaxNameLength = 64;

// Saved/VoxelWorlds/<seed>.vxmarks.json, absolute. Exists whether or not the
// file does.
VOXELEARTHUI_API FString PathForSeed(uint64 Seed);

// Everything readable in the file for this seed, oldest first. An absent file,
// unparsable JSON, a wrong or future schema version, or a record with a
// non-finite coordinate all yield the marks that WERE readable rather than an
// error -- see the header note.
VOXELEARTHUI_API TArray<FVoxelMapMark> Load(uint64 Seed);

// Write-through. Called on every add, rename and remove, because the
// alternative is a list that survives a clean quit and not a crash. Logs
// `VoxelMap: %d mark(s) saved to %s` on success and a warning on failure;
// returns false rather than throwing away the caller's in-memory list.
VOXELEARTHUI_API bool Save(uint64 Seed, const TArray<FVoxelMapMark>& Marks);
} // namespace VoxelMapMarks
