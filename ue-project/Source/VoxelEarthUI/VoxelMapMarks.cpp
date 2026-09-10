#include "VoxelMapMarks.h"

#include "VoxelEarthUI.h" // LogVoxelUI

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace VoxelMapMarksDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// The field names, once. They are read in two functions and a typo in one of
// them is a mark list that saves and never loads -- which is exactly the shape
// of silent failure this project keeps paying for.
const TCHAR* const kFieldVersion = TEXT("version");
const TCHAR* const kFieldSeed    = TEXT("seed");
const TCHAR* const kFieldMarks   = TEXT("marks");
const TCHAR* const kFieldName    = TEXT("name");
const TCHAR* const kFieldX       = TEXT("x");
const TCHAR* const kFieldY       = TEXT("y");
const TCHAR* const kFieldIcon    = TEXT("icon");
const TCHAR* const kFieldDay     = TEXT("day");
const TCHAR* const kFieldCreated = TEXT("created");

// Clamped and stripped of anything that would make the label unreadable on the
// sheet. Applied on the way IN as well as on the way out, because a hand-edited
// file is exactly where a 40 kB name or an embedded newline comes from.
FString SanitiseName(const FString& In)
{
	FString Out = In;
	Out.ReplaceInline(TEXT("\r"), TEXT(" "));
	Out.ReplaceInline(TEXT("\n"), TEXT(" "));
	Out.ReplaceInline(TEXT("\t"), TEXT(" "));
	Out.TrimStartAndEndInline();
	if (Out.Len() > VoxelMapMarks::kMaxNameLength)
	{
		// Left(), not LeftInline(): the inline form's second parameter changed
		// from a bool to EAllowShrinking across recent engine versions and the
		// bool overload is deprecated. One allocation on a path that runs once
		// per mark edit is not worth a version-sensitive call.
		Out = Out.Left(VoxelMapMarks::kMaxNameLength);
	}
	return Out;
}
} // namespace VoxelMapMarksDetail

FString VoxelMapMarks::PathForSeed(uint64 Seed)
{
	// Beside <seed>.vxlog and <seed>.vxhydro, and named the same way. See the
	// header for why the seed is the key.
	return FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("VoxelWorlds")
		/ FString::Printf(TEXT("%llu.vxmarks.json"), (unsigned long long)Seed));
}

TArray<FVoxelMapMark> VoxelMapMarks::Load(uint64 Seed)
{
	using namespace VoxelMapMarksDetail;

	TArray<FVoxelMapMark> Out;
	const FString Path = PathForSeed(Seed);

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		// THE ORDINARY ANSWER, not a failure. Logged at Verbose so a first run
		// does not put a line in the log for every world that has no marks yet,
		// but a session hunting a missing list can still see the path it looked
		// at.
		UE_LOG(LogVoxelUI, Verbose, TEXT("VoxelMap: no marks file at %s."), *Path);
		return Out;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogVoxelUI, Warning,
		       TEXT("VoxelMap: marks file %s is not valid JSON; starting with no marks. The file is left ")
		       TEXT("on disk untouched until the next save."),
		       *Path);
		return Out;
	}

	// A MISSING VERSION READS AS 0, not as the current one. Nothing has ever
	// written a file without it, so 0 means damaged rather than legacy -- but
	// the check below refuses it the same way either way, which is the safe
	// direction.
	// TryGetNumberField, not GetIntegerField: the non-Try accessors log an
	// error of their own when the field is absent, which would turn an
	// ordinary damaged file into a red line in the log for something this
	// function already handles.
	int32 Version = 0;
	Root->TryGetNumberField(kFieldVersion, Version);
	if (Version != kFormatVersion)
	{
		UE_LOG(LogVoxelUI, Warning,
		       TEXT("VoxelMap: marks file %s is schema version %d, this build writes %d; ignoring it rather ")
		       TEXT("than guessing. The file is not overwritten until the player saves a mark."),
		       *Path, Version, kFormatVersion);
		return Out;
	}

	// The seed inside the file is a cross-check, not the key -- the key is the
	// filename. A mismatch means somebody copied a file between worlds, and
	// marks from another world are exactly the "confidently wrong map" the map
	// screen's seed gate exists to prevent.
	FString SeedText;
	if (Root->TryGetStringField(kFieldSeed, SeedText))
	{
		const FString Expected = FString::Printf(TEXT("%llu"), (unsigned long long)Seed);
		if (!SeedText.Equals(Expected))
		{
			UE_LOG(LogVoxelUI, Warning,
			       TEXT("VoxelMap: marks file %s records seed %s but this world is %s; ignoring it."),
			       *Path, *SeedText, *Expected);
			return Out;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Root->TryGetArrayField(kFieldMarks, Rows) || Rows == nullptr)
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelMap: marks file %s has no '%s' array."), *Path, kFieldMarks);
		return Out;
	}

	int32 Skipped = 0;
	for (const TSharedPtr<FJsonValue>& Row : *Rows)
	{
		if (Out.Num() >= kMaxMarks)
		{
			Skipped += Rows->Num() - Out.Num();
			break;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Row.IsValid() || !Row->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
		{
			++Skipped;
			continue;
		}

		double X = 0.0;
		double Y = 0.0;
		if (!(*Object)->TryGetNumberField(kFieldX, X) || !(*Object)->TryGetNumberField(kFieldY, Y))
		{
			++Skipped;
			continue;
		}
		// A NON-FINITE COORDINATE IS THE ONE THAT MUST NOT GET THROUGH. It
		// survives every arithmetic step of the map transform and comes out as
		// a NaN vertex position, which in Slate is a triangle that swallows the
		// whole draw batch -- a corrupt line in a text file becoming a blank
		// screen with no error.
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y))
		{
			++Skipped;
			continue;
		}

		FVoxelMapMark Mark;
		Mark.WorldXY = FVector2D(X, Y);
		FString Name;
		(*Object)->TryGetStringField(kFieldName, Name);
		Mark.Name = FText::FromString(SanitiseName(Name));
		FString Icon;
		if ((*Object)->TryGetStringField(kFieldIcon, Icon) && !Icon.IsEmpty())
		{
			Mark.Icon = FName(*Icon);
		}
		int32 Day = 0;
		(*Object)->TryGetNumberField(kFieldDay, Day);
		Mark.DayPlaced = Day;
		// int64 through a double is exact to 2^53, which is the year 285,000,000
		// in Unix seconds. TryGetNumberField's int64 overload is used where it
		// exists; the double path is the one every JSON number takes anyway.
		double Created = 0.0;
		(*Object)->TryGetNumberField(kFieldCreated, Created);
		Mark.CreatedUnixTime = FMath::IsFinite(Created) ? int64(Created) : 0;

		Out.Add(MoveTemp(Mark));
	}

	const FString SkipNote = Skipped > 0
		? FString::Printf(TEXT(" (%d unreadable record(s) skipped)"), Skipped)
		: FString();
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelMap: %d mark(s) loaded from %s%s."), Out.Num(), *Path, *SkipNote);
	return Out;
}

bool VoxelMapMarks::Save(uint64 Seed, const TArray<FVoxelMapMark>& Marks)
{
	using namespace VoxelMapMarksDetail;

	const FString Path = PathForSeed(Seed);

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(kFieldVersion, kFormatVersion);
	// AS A STRING. The session plan's schema rule, verbatim: "Encode full-width
	// IDs/seeds as strings in JSON." A uint64 seed does not survive a JSON
	// double, and a seed is an identifier rather than a quantity.
	Root->SetStringField(kFieldSeed, FString::Printf(TEXT("%llu"), (unsigned long long)Seed));

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Min(Marks.Num(), kMaxMarks));
	for (const FVoxelMapMark& Mark : Marks)
	{
		if (Rows.Num() >= kMaxMarks)
		{
			break;
		}
		if (!FMath::IsFinite(Mark.WorldXY.X) || !FMath::IsFinite(Mark.WorldXY.Y))
		{
			continue;
		}
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(kFieldName, SanitiseName(Mark.Name.ToString()));
		// UNREAL UNITS, said out loud in the file itself is not possible in
		// JSON, so it is said here and in the header: x and y are world UU
		// (1 uu = 1 cm), NOT metres and NOT sheet pixels. The mock's marks were
		// in its own 4200x2800 image space, which is meaningless outside it.
		Object->SetNumberField(kFieldX, Mark.WorldXY.X);
		Object->SetNumberField(kFieldY, Mark.WorldXY.Y);
		if (!Mark.Icon.IsNone())
		{
			Object->SetStringField(kFieldIcon, Mark.Icon.ToString());
		}
		Object->SetNumberField(kFieldDay, Mark.DayPlaced);
		Object->SetNumberField(kFieldCreated, double(Mark.CreatedUnixTime));
		Rows.Add(MakeShared<FJsonValueObject>(Object));
	}
	Root->SetArrayField(kFieldMarks, Rows);

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelMap: failed to serialise %d mark(s) for %s."), Rows.Num(), *Path);
		return false;
	}

	// The directory is the one .vxlog and .vxhydro already live in, but a fresh
	// checkout that has never run the game does not have it.
	const FString Dir = FPaths::GetPath(Path);
	if (!IFileManager::Get().DirectoryExists(*Dir))
	{
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
	}

	if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelMap: could not write %s; %d mark(s) are session-only."),
		       *Path, Rows.Num());
		return false;
	}

	UE_LOG(LogVoxelUI, Log, TEXT("VoxelMap: %d mark(s) saved to %s"), Rows.Num(), *Path);
	return true;
}
