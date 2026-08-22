#pragma once
// The menu background rotation: enumerating Content/UI/Backgrounds, decoding
// one JPEG at a time on a worker, and handing Slate a brush for it.
//
// WHY THIS DOES ITS OWN DECODING rather than letting an FSlateStyleSet's
// IMAGE_BRUSH do it. Two reasons, and the second is the load-bearing one.
//
//   * A style set resolves images through the Slate resource manager, which
//     decides for itself when the bytes get turned into a texture. These are
//     1920-wide JPEGs; the decode is tens of milliseconds. Doing it on the
//     game thread at an unpredictable moment is exactly the demand-driven
//     spike doctrine 5 exists to forbid ("everything expensive is budgeted,
//     never demand-driven").
//   * The loading screen crossfades between two of them WHILE the world is
//     streaming and the frame budget is already gone. Controlling when the
//     decode happens -- on a background task, with the game thread doing only
//     the upload -- is the difference between a crossfade and a hitch.
//
// GRACEFUL DEGRADATION IS A FIRST-CLASS PATH. Missing art is not an error
// state to assert on: a shallow checkout, a packaging misconfiguration or
// -VoxelUINoAssets can all produce it, and a front end that fails to draw is a
// game that fails to start. So HasAnyBackground() is a question the menu asks
// and answers by drawing itself differently -- see SVoxelMainMenu, which also
// drops the 55% tint when there is no art, exactly as MainMenu.gd's
// _setup_background early-return does.

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "UObject/Object.h"
#include "UObject/StrongObjectPtr.h"
#include "VoxelUIAssetLibrary.generated.h"

// Holds the decoded textures alive.
//
// UTexture2D::CreateTransient produces an object with no outer package and no
// reference from anywhere, which the garbage collector is entirely within its
// rights to take mid-crossfade. A UPROPERTY array in a rooted UObject is the
// ordinary way to say "these are in use"; without it the failure is an
// intermittent black rectangle that only shows up under memory pressure.
UCLASS()
class UVoxelUITextureCache : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> Textures;
};

class VOXELEARTHUI_API FVoxelUIAssetLibrary
{
public:
	static FVoxelUIAssetLibrary& Get();
	static void Shutdown();

	// Enumerates Content/UI/Backgrounds/*.{jpg,jpeg,png}. Called once at
	// startup; MainMenu.gd re-scans on every launch and so does this, so
	// dropping a new image into the folder needs no code change.
	void RescanBackgrounds();

	int32 NumBackgrounds() const { return Entries.Num(); }
	bool HasAnyBackground() const { return Entries.Num() > 0; }

	// Reshuffles the rotation order. Called per screen-show, matching the
	// Godot build, which shuffles its background pool and both text lists
	// fresh on every _show_loading_screen so a player rarely sees the same
	// opener twice.
	void ShuffleOrder(FRandomStream& Stream);

	// The brush for the Index'th image IN SHUFFLED ORDER, or null while the
	// decode is still in flight. Null is an ordinary answer, not a failure:
	// the caller simply draws nothing that frame and asks again next frame.
	// Kicks the decode on first request.
	const FSlateBrush* RequestBackground(int32 Index);

private:
	struct FEntry
	{
		FString Path;
		// Decoded lazily. Both flags are game-thread-only.
		bool bDecodeStarted = false;
		bool bDecodeFailed = false;
		TSharedPtr<FSlateBrush> Brush;
	};

	void BeginDecode(int32 EntryIndex);
	void FinishDecode(int32 EntryIndex, TArray<uint8>&& Pixels, int32 Width, int32 Height);

	TArray<FEntry> Entries;
	// Indices into Entries, in the current shuffled order.
	TArray<int32> Order;
	TStrongObjectPtr<UVoxelUITextureCache> Cache;
};
