#include "VoxelSaveLibrary.h"

#include "VoxelDebug.h" // LogVoxelEdit
#include "VoxelWorldSubsystem.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

// After the UE headers, matching every other .cpp in this module that reaches
// into voxel-core (VoxelAgentSubsystem.cpp, VoxelCoverVerify.cpp, ...).
#include "voxelcore/core.h"    // kWorldGenVersion -- stamped into meta.json
#include "voxelcore/editlog.h" // EditLog::kFormatVersion -- stamped beside it

namespace VoxelSaveDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

const TCHAR* const kMetaFile = TEXT("meta.json");
const TCHAR* const kWorldFile = TEXT("world.vxlog");

FString SavesRoot()
{
	return FPaths::ProjectSavedDir() / TEXT("SaveGames");
}

// The slug that this session's saves belong to. Deliberately process-global
// rather than a member of anything: it outlives the menu widget that sets it
// and is read by the world subsystem's shutdown path, and threading it through
// both would mean giving the world subsystem a dependency on the save layer.
FString GActiveSlug;

// Reads meta.json. Returns false on anything unreadable rather than
// half-populating -- a save row built from a corrupt file would advertise a
// world that will not open.
bool ReadMeta(const FString& Slug, VoxelSave::FSaveInfo& Out)
{
	const FString Path = VoxelSave::SaveDirectory(Slug) / kMetaFile;
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("Save '%s': meta.json is not valid JSON; skipping it."), *Slug);
		return false;
	}

	Out.Slug = Slug;
	Out.DisplayName = Root->GetStringField(TEXT("display_name"));
	Out.TimestampIso = Root->GetStringField(TEXT("timestamp"));
	// GetNumberField returns double. A uint64 seed above 2^53 would lose
	// precision through it, so the seed travels as a decimal STRING -- the
	// same reason the engine writes large ids as strings in its own JSON.
	FString SeedString;
	if (Root->TryGetStringField(TEXT("seed"), SeedString))
	{
		Out.Seed = FCString::Strtoui64(*SeedString, nullptr, 10);
	}
	Out.UnixTime = int64(Root->GetNumberField(TEXT("unix_time")));
	Out.PlayTimeSeconds = int32(Root->GetNumberField(TEXT("play_time_seconds")));
	Out.EditCount = int64(Root->GetNumberField(TEXT("edit_count")));
	Out.bIsAutosave = Root->GetBoolField(TEXT("is_autosave"));

	// TryGetNumberField, not GetNumberField: an absent field leaves the 0 that
	// FSaveInfo documents as UNSTAMPED. A save written before these fields
	// existed must still list and still load -- see FSaveInfo's comment on why
	// 0 is a third answer rather than a bad version.
	Root->TryGetNumberField(TEXT("meta_version"), Out.MetaVersion);
	Root->TryGetNumberField(TEXT("world_gen_version"), Out.WorldGenVersion);
	Root->TryGetNumberField(TEXT("edit_log_format_version"), Out.EditLogFormatVersion);

	const TSharedPtr<FJsonObject>* Position = nullptr;
	if (Root->TryGetObjectField(TEXT("player_position"), Position) && Position != nullptr)
	{
		Out.PlayerPosition.X = (*Position)->GetNumberField(TEXT("x"));
		Out.PlayerPosition.Y = (*Position)->GetNumberField(TEXT("y"));
		Out.PlayerPosition.Z = (*Position)->GetNumberField(TEXT("z"));
	}
	const TSharedPtr<FJsonObject>* Rotation = nullptr;
	if (Root->TryGetObjectField(TEXT("player_rotation"), Rotation) && Rotation != nullptr)
	{
		Out.PlayerRotation.Pitch = (*Rotation)->GetNumberField(TEXT("pitch"));
		Out.PlayerRotation.Yaw = (*Rotation)->GetNumberField(TEXT("yaw"));
		Out.PlayerRotation.Roll = (*Rotation)->GetNumberField(TEXT("roll"));
	}
	return true;
}
} // namespace VoxelSaveDetail

namespace VoxelSave
{
FString SaveDirectory(const FString& Slug)
{
	return VoxelSaveDetail::SavesRoot() / Slug;
}

FString WorldLogPath(const FString& Slug)
{
	return SaveDirectory(Slug) / VoxelSaveDetail::kWorldFile;
}

FString Slugify(const FString& DisplayName)
{
	// GameState._slugify: lowercase, non-alphanumerics to underscore, runs
	// collapsed, ends trimmed. "My Save 1!" -> "my_save_1".
	FString Out;
	Out.Reserve(DisplayName.Len());
	bool bLastWasUnderscore = true; // true, so a leading run is dropped
	for (const TCHAR Character : DisplayName)
	{
		const TCHAR Lower = FChar::ToLower(Character);
		if (FChar::IsAlnum(Lower))
		{
			Out.AppendChar(Lower);
			bLastWasUnderscore = false;
		}
		else if (!bLastWasUnderscore)
		{
			Out.AppendChar(TEXT('_'));
			bLastWasUnderscore = true;
		}
	}
	while (Out.EndsWith(TEXT("_"), ESearchCase::CaseSensitive))
	{
		Out.LeftChopInline(1);
	}
	// An all-punctuation name would otherwise slug to "" and write into the
	// saves root itself, which is a directory full of other people's saves.
	return Out.IsEmpty() ? TEXT("untitled") : Out;
}

TArray<FSaveInfo> List()
{
	TArray<FSaveInfo> Out;
	const FString Root = VoxelSaveDetail::SavesRoot();

	TArray<FString> Directories;
	IFileManager::Get().FindFiles(Directories, *(Root / TEXT("*")), /*Files=*/false, /*Directories=*/true);
	for (const FString& Slug : Directories)
	{
		FSaveInfo Info;
		if (VoxelSaveDetail::ReadMeta(Slug, Info))
		{
			Out.Add(MoveTemp(Info));
		}
	}

	// NEWEST FIRST. This ordering is the definition of CONTINUE (MainMenu.gd
	// takes list_save_files()[0]), so it is a behaviour and not a
	// presentation choice.
	Out.Sort([](const FSaveInfo& A, const FSaveInfo& B) { return A.UnixTime > B.UnixTime; });
	return Out;
}

bool Write(const UVoxelWorldSubsystem& World, const FString& DisplayName, bool bIsAutosave,
           const FTransform& PlayerTransform, int32 PlayTimeSeconds)
{
	const FString Name = DisplayName.TrimStartAndEnd().IsEmpty() ? TEXT("Untitled") : DisplayName.TrimStartAndEnd();
	FString Slug = Slugify(Name);

	// Collisions get a suffix rather than a silent overwrite. Overwriting IS
	// wanted when the player saves over their own file, which they do by
	// reusing the same slug -- so the disambiguation only kicks in when the
	// existing directory belongs to a DIFFERENT display name.
	{
		FSaveInfo Existing;
		int32 Attempt = 2;
		while (VoxelSaveDetail::ReadMeta(Slug, Existing) && Existing.DisplayName != Name && Attempt < 1000)
		{
			Slug = Slugify(Name) + FString::Printf(TEXT("-%d"), Attempt++);
		}
	}

	const FString Directory = SaveDirectory(Slug);
	IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);

	// THE WORLD GOES FIRST. If serialising the edit log fails, no meta.json is
	// written, so the menu never lists a save whose world is missing --
	// a row that opens an empty world is worse than a save that visibly did
	// not happen.
	if (!World.SaveWorldToPath(WorldLogPath(Slug)))
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("SaveGame '%s': the world log could not be written; no save was created."), *Slug);
		return false;
	}

	const FDateTime Now = FDateTime::UtcNow();
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("display_name"), Name);
	Root->SetStringField(TEXT("timestamp"), Now.ToString(TEXT("%Y-%m-%d %H:%M")));
	Root->SetNumberField(TEXT("unix_time"), double(Now.ToUnixTimestamp()));
	// A decimal string, not a number: JSON numbers are doubles and this project
	// uses full-width uint64 seeds.
	Root->SetStringField(TEXT("seed"), FString::Printf(TEXT("%llu"), (unsigned long long)World.GetSeed()));
	Root->SetNumberField(TEXT("play_time_seconds"), double(PlayTimeSeconds));
	Root->SetNumberField(TEXT("edit_count"), double(World.GetLogSize()));
	Root->SetBoolField(TEXT("is_autosave"), bIsAutosave);

	// THE VERSION STAMPS. meta.json had none, which meant that when the
	// world.vxlog beside it stopped loading, nothing on disk recorded WHICH
	// version had written it -- so "older build, bytes intact" and "damaged"
	// were indistinguishable, and no migration could ever be written because
	// there was nothing to migrate FROM. All three are plain JSON numbers and
	// safely below 2^53, unlike the seed above.
	Root->SetNumberField(TEXT("meta_version"), double(kMetaVersion));
	Root->SetNumberField(TEXT("world_gen_version"), double(vxc::kWorldGenVersion));
	Root->SetNumberField(TEXT("edit_log_format_version"), double(vxc::EditLog::kFormatVersion));

	const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), PlayerTransform.GetLocation().X);
	Position->SetNumberField(TEXT("y"), PlayerTransform.GetLocation().Y);
	Position->SetNumberField(TEXT("z"), PlayerTransform.GetLocation().Z);
	Root->SetObjectField(TEXT("player_position"), Position);

	const FRotator Rotation = PlayerTransform.Rotator();
	const TSharedRef<FJsonObject> RotationObject = MakeShared<FJsonObject>();
	RotationObject->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	RotationObject->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	RotationObject->SetNumberField(TEXT("roll"), Rotation.Roll);
	Root->SetObjectField(TEXT("player_rotation"), RotationObject);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer) || !FFileHelper::SaveStringToFile(Json, *(Directory / VoxelSaveDetail::kMetaFile)))
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("SaveGame '%s': the world was written but meta.json was not."), *Slug);
		return false;
	}

	VoxelSaveDetail::GActiveSlug = Slug;
	UE_LOG(LogVoxelEdit, Log, TEXT("SaveGame '%s' (%s): %llu edit(s), seed %llu, at (%.0f, %.0f, %.0f)."), *Slug,
	       bIsAutosave ? TEXT("autosave") : TEXT("named"), (unsigned long long)World.GetLogSize(),
	       (unsigned long long)World.GetSeed(), PlayerTransform.GetLocation().X, PlayerTransform.GetLocation().Y,
	       PlayerTransform.GetLocation().Z);

	if (bIsAutosave)
	{
		PruneAutosaves();
	}
	return true;
}

bool Delete(const FString& Slug)
{
	const FString Directory = SaveDirectory(Slug);
	if (!IFileManager::Get().DirectoryExists(*Directory))
	{
		return false;
	}
	const bool bOk = IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists=*/false, /*Tree=*/true);
	UE_LOG(LogVoxelEdit, Log, TEXT("DeleteSave '%s': %s."), *Slug, bOk ? TEXT("removed") : TEXT("FAILED"));
	if (bOk && VoxelSaveDetail::GActiveSlug == Slug)
	{
		VoxelSaveDetail::GActiveSlug.Reset();
	}
	return bOk;
}

int32 PruneAutosaves(int32 MaxAutosaves)
{
	if (MaxAutosaves <= 0)
	{
		return 0;
	}
	TArray<FSaveInfo> Autosaves = List().FilterByPredicate([](const FSaveInfo& Info) { return Info.bIsAutosave; });
	// List() is newest-first, so everything past the cap is the oldest --
	// GameState.gd's FIFO rule, and the reason is_autosave is recorded at all:
	// NAMED SAVES ARE NEVER PRUNED, however many there are.
	int32 Removed = 0;
	for (int32 Index = MaxAutosaves; Index < Autosaves.Num(); ++Index)
	{
		if (Delete(Autosaves[Index].Slug))
		{
			++Removed;
		}
	}
	if (Removed > 0)
	{
		UE_LOG(LogVoxelEdit, Log, TEXT("PruneAutosaves: removed %d autosave(s) beyond the cap of %d."), Removed,
		       MaxAutosaves);
	}
	return Removed;
}

int64 SecondsSinceLastSave()
{
	const TArray<FSaveInfo> Saves = List();
	if (Saves.Num() == 0)
	{
		// GameState.seconds_since_last_save() returns 999999 when there has
		// never been a save, so that a "> 30 seconds" staleness test passes.
		// Ported as-is rather than as a TOptional, because every caller wants
		// exactly that behaviour and none of them wants to write the branch.
		return 999999;
	}
	return FMath::Max<int64>(0, FDateTime::UtcNow().ToUnixTimestamp() - Saves[0].UnixTime);
}

const FString& GetActiveSlug()
{
	return VoxelSaveDetail::GActiveSlug;
}

void SetActiveSlug(const FString& Slug)
{
	VoxelSaveDetail::GActiveSlug = Slug;
}
} // namespace VoxelSave

// --- Console commands -------------------------------------------------------
//
// WHY THESE EXIST AT ALL. In the Godot build, the only way a player creates a
// named save is the pause menu's SAVE button. The pause menu is an explicit
// follow-up to this pass (docs/front-end-plan.md), which would leave the main
// menu's CONTINUE and LOAD GAME permanently greyed out -- correct behaviour,
// and impossible to test or to use. These three commands close that gap
// without pre-building half the pause menu, and the SAVE dialog binds to the
// same VoxelSave::Write when it lands.
//
// Registered here, beside the functions they call, following the 43
// FAutoConsoleCommand registrations already spread through this module.

namespace VoxelSaveCommands
{
// The player's transform, or identity if there is no pawn yet. Identity is a
// legitimate answer, not a failure: saving from the main menu (no pawn) should
// still record a world, and RestartPlayer's ordinary spawn logic will place
// the player when it is loaded.
FTransform PlayerTransformOrIdentity(UWorld* World)
{
	if (World != nullptr)
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (const APawn* Pawn = PC->GetPawn())
			{
				return Pawn->GetActorTransform();
			}
		}
	}
	return FTransform::Identity;
}

FAutoConsoleCommandWithWorldAndArgs GSaveGameCommand(
	TEXT("voxel.SaveGame"),
	TEXT("voxel.SaveGame <name...> -- write a named save (Saved/SaveGames/<slug>/). ")
	TEXT("With no name, uses the current date and time."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UVoxelWorldSubsystem* Voxels = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
			if (Voxels == nullptr)
			{
				UE_LOG(LogVoxelEdit, Error, TEXT("voxel.SaveGame: no voxel world subsystem."));
				return;
			}
			// Joined with spaces so `voxel.SaveGame Copper Isles Day 1` does
			// the obvious thing rather than saving a file called "Copper".
			const FString Name = Args.Num() > 0 ? FString::Join(Args, TEXT(" "))
			                                    : FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M"));
			VoxelSave::Write(*Voxels, Name, /*bIsAutosave=*/false,
			                 VoxelSaveCommands::PlayerTransformOrIdentity(World), /*PlayTimeSeconds=*/0);
		}));

FAutoConsoleCommand GListSavesCommand(
	TEXT("voxel.ListSaves"),
	TEXT("List every named save, newest first -- the same order the main menu shows."),
	FConsoleCommandDelegate::CreateStatic(
		[]()
		{
			const TArray<VoxelSave::FSaveInfo> Saves = VoxelSave::List();
			if (Saves.Num() == 0)
			{
				UE_LOG(LogVoxelEdit, Display, TEXT("voxel.ListSaves: no saves in %s."),
				       *(FPaths::ProjectSavedDir() / TEXT("SaveGames")));
				return;
			}
			UE_LOG(LogVoxelEdit, Display, TEXT("voxel.ListSaves: %d save(s), newest first:"), Saves.Num());
			for (const VoxelSave::FSaveInfo& Info : Saves)
			{
				UE_LOG(LogVoxelEdit, Display,
				       TEXT("  %-24s %s  seed %llu  %lld edit(s)  X %.0f Y %.0f Z %.0f%s"), *Info.Slug,
				       *Info.TimestampIso, (unsigned long long)Info.Seed, (long long)Info.EditCount,
				       Info.PlayerPosition.X, Info.PlayerPosition.Y, Info.PlayerPosition.Z,
				       Info.bIsAutosave ? TEXT("  [auto]") : TEXT(""));
			}
		}));

// FAutoConsoleCommand, NOT FAutoConsoleCommandWithArgs -- the latter does not
// exist. IConsoleManager.h ships FAutoConsoleCommand{,WithWorld,WithWorldAndArgs,
// WithArgsAndOutputDevice,WithOutputDevice,WithWorldArgsAndOutputDevice}, and
// plain FAutoConsoleCommand already has an overload taking exactly the
// FConsoleCommandWithArgsDelegate this builds. The mistake broke the whole
// module build on main; the two commands above it use real type names, which is
// why they compiled and this did not.
FAutoConsoleCommand GDeleteSaveCommand(
	TEXT("voxel.DeleteSave"),
	TEXT("voxel.DeleteSave <slug> -- permanently remove one save directory."),
	FConsoleCommandWithArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args)
		{
			if (Args.Num() != 1)
			{
				UE_LOG(LogVoxelEdit, Error, TEXT("voxel.DeleteSave: expected exactly one slug (see voxel.ListSaves)."));
				return;
			}
			if (!VoxelSave::Delete(Args[0]))
			{
				UE_LOG(LogVoxelEdit, Error, TEXT("voxel.DeleteSave: no save called '%s'."), *Args[0]);
			}
		}));
} // namespace VoxelSaveCommands
