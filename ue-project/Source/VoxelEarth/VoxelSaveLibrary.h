#pragma once
// Named saves: Saved/SaveGames/<slug>/{meta.json, world.vxlog}.
//
// WHAT THIS IS AND IS NOT. The project already had world persistence -- one
// .vxlog per seed at Saved/VoxelWorlds/<seed>.vxlog, written on shutdown and
// by voxel.SaveWorld. What it did NOT have was any notion of a SAVE: a thing
// with a name, a timestamp, a player position, and a sibling you could choose
// between. The main menu's CONTINUE / LOAD GAME / DELETE rows need exactly
// that, so this adds it -- as a thin layer over the existing edit log rather
// than as a second persistence system.
//
// A NAMESPACE, NOT A UOBJECT. There is no UGameInstance subclass in this
// project and no UGameInstanceSubsystem anywhere in it (every subsystem here
// is a world subsystem), so introducing the first one to hold four file
// operations would be inventing a lifetime problem to solve a filing problem.
// These are free functions over the filesystem.
//
// PORTED FROM scripts/GameState.gd, whose four load-bearing behaviours all
// survive: newest-first ordering (so CONTINUE is literally List()[0]), the
// five-autosave FIFO cap with named saves never pruned, the slugify rule, and
// seconds-since-last-save for the pause menu's autosave-if-stale gate.
//
// THE ONE THING THAT DOES NOT SURVIVE IS PER-SAVE SEEDS, and it is worth being
// precise about why rather than discovering it later. UVoxelWorldSubsystem
// resolves Seed once in Initialize, BEFORE Impl is constructed, and the seed is
// baked into the amplifier at that moment -- so a save recorded under a
// different seed cannot be opened without rebuilding the whole voxel world.
// The Godot menu has no seed picker, so a 1:1 clone does not need one: NEW GAME
// uses the run's seed and every save shares it. A save whose recorded seed
// differs (reachable only via -VoxelSeed=) is listed but not loadable, with the
// reason shown in place of its detail line. Extracting a
// ConstructImplForSeed() to lift that restriction is a follow-up, not this
// pass.

#include "CoreMinimal.h"

class UVoxelWorldSubsystem;

namespace VoxelSave
{
// One row of the LOAD GAME list, read from a save's meta.json.
struct VOXELEARTH_API FSaveInfo
{
	FString Slug;          // directory name under Saved/SaveGames
	FString DisplayName;   // what the player typed
	FString TimestampIso;  // human-readable, as written
	int64 UnixTime = 0;    // what the newest-first sort actually uses
	uint64 Seed = 0;
	FVector PlayerPosition = FVector::ZeroVector;
	FRotator PlayerRotation = FRotator::ZeroRotator;
	int32 PlayTimeSeconds = 0;
	int64 EditCount = 0;
	bool bIsAutosave = false;
};

// Every save on disk, NEWEST FIRST. That ordering is not cosmetic: MainMenu.gd
// implements CONTINUE as `GameState.list_save_files()[0]`, and this port keeps
// that, so the sort IS the definition of "continue".
VOXELEARTH_API TArray<FSaveInfo> List();

// Directory for a slug, whether or not it exists.
VOXELEARTH_API FString SaveDirectory(const FString& Slug);
VOXELEARTH_API FString WorldLogPath(const FString& Slug);

// Writes meta.json and world.vxlog. PlayerTransform is captured by the caller
// (the front end or the console command) because this layer deliberately knows
// nothing about pawns.
//
// Returns false and writes NOTHING if the edit log could not be serialised --
// a meta.json without its world beside it would show up in the menu as a
// loadable save that opens an empty world.
VOXELEARTH_API bool Write(const UVoxelWorldSubsystem& World, const FString& DisplayName, bool bIsAutosave,
                          const FTransform& PlayerTransform, int32 PlayTimeSeconds);

// Removes the whole directory. Returns false if it did not exist.
VOXELEARTH_API bool Delete(const FString& Slug);

// "My Save 1!" -> "my_save_1"; empty or all-punctuation -> "untitled".
// Collisions are resolved by the caller (Write appends -2, -3, ...), because a
// slugify that silently renamed would make "save over my old file" impossible.
VOXELEARTH_API FString Slugify(const FString& DisplayName);

// FIFO eviction of autosaves beyond MaxAutosaves. NAMED SAVES ARE NEVER
// PRUNED -- GameState.gd's rule, and the reason the flag is in meta.json at
// all. Returns how many were removed.
VOXELEARTH_API int32 PruneAutosaves(int32 MaxAutosaves = 5);

// Seconds since the newest save was written, or a large sentinel if there has
// never been one. GameState.seconds_since_last_save() returns 999999 for that
// case and the pause menu compares it against a 30 s window; both come across
// unchanged when that menu lands.
VOXELEARTH_API int64 SecondsSinceLastSave();

// The slug the running session is attached to, if any. Set by Write and by the
// front end when it opens a save; read by the autosave-on-shutdown path so a
// session that came from a named save is written back to that save rather than
// to the seed-derived default.
VOXELEARTH_API const FString& GetActiveSlug();
VOXELEARTH_API void SetActiveSlug(const FString& Slug);
} // namespace VoxelSave
