#include "VoxelUIAssetLibrary.h"

#include "VoxelEarthUI.h"
#include "VoxelFrontEndSwitches.h"

#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Tasks/Task.h"

namespace VoxelUIAssetDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

TUniquePtr<FVoxelUIAssetLibrary> GInstance;

const TCHAR* const kBackgroundsSubdir = TEXT("UI/Backgrounds");

// Decode one image file to BGRA8. Runs on a worker; touches nothing but its
// arguments. Returns false on any failure, which the caller reports once and
// then treats as "this file is not in the rotation".
bool DecodeToBGRA(const FString& Path, TArray<uint8>& OutPixels, int32& OutWidth, int32& OutHeight)
{
	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *Path))
	{
		return false;
	}

	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	// Sniffed from the bytes rather than trusted from the extension: a .jpg
	// that is really a PNG is a thing that happens to art folders, and the
	// failure without this is a black rectangle with no explanation.
	const EImageFormat Format = Module.DetectImageFormat(FileBytes.GetData(), FileBytes.Num());
	if (Format == EImageFormat::Invalid)
	{
		return false;
	}
	TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Format);
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(FileBytes.GetData(), FileBytes.Num()))
	{
		return false;
	}
	if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, OutPixels))
	{
		return false;
	}
	OutWidth = Wrapper->GetWidth();
	OutHeight = Wrapper->GetHeight();
	return OutWidth > 0 && OutHeight > 0 && OutPixels.Num() >= OutWidth * OutHeight * 4;
}
} // namespace VoxelUIAssetDetail

FVoxelUIAssetLibrary& FVoxelUIAssetLibrary::Get()
{
	if (!VoxelUIAssetDetail::GInstance)
	{
		VoxelUIAssetDetail::GInstance = MakeUnique<FVoxelUIAssetLibrary>();
		VoxelUIAssetDetail::GInstance->Cache.Reset(NewObject<UVoxelUITextureCache>());
		VoxelUIAssetDetail::GInstance->RescanBackgrounds();
	}
	return *VoxelUIAssetDetail::GInstance;
}

void FVoxelUIAssetLibrary::Shutdown()
{
	VoxelUIAssetDetail::GInstance.Reset();
}

void FVoxelUIAssetLibrary::RescanBackgrounds()
{
	Entries.Reset();
	Order.Reset();

	if (FVoxelFrontEndSwitches::Get().bNoAssets)
	{
		// -VoxelUINoAssets. Makes the degraded path screenshot-testable rather
		// than theoretical, which matters because it is the path nobody
		// exercises until it is the only one they have.
		UE_LOG(LogVoxelUI, Log, TEXT("Menu backgrounds: skipped (-VoxelUINoAssets)."));
		return;
	}

	const FString Dir = FPaths::ProjectContentDir() / VoxelUIAssetDetail::kBackgroundsSubdir;
	TArray<FString> Found;
	for (const TCHAR* Pattern : {TEXT("*.jpg"), TEXT("*.jpeg"), TEXT("*.png")})
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / Pattern), /*Files=*/true, /*Directories=*/false);
		for (const FString& Name : Names)
		{
			Found.Add(Dir / Name);
		}
	}
	// Sorted so the pre-shuffle order is deterministic. It matters for the
	// -VoxelHourglassShot/-VoxelMenuShot captures, which seed the shuffle from
	// a constant under -unattended precisely so two runs produce comparable
	// images; an order that depended on the filesystem's enumeration would
	// undo that.
	Found.Sort();

	for (const FString& Path : Found)
	{
		FEntry Entry;
		Entry.Path = Path;
		Entries.Add(MoveTemp(Entry));
	}
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		Order.Add(Index);
	}

	if (Entries.Num() == 0)
	{
		// One warning, naming the directory, because "the menu is black" is
		// otherwise a mystery with no thread to pull.
		UE_LOG(LogVoxelUI, Warning,
		       TEXT("Menu backgrounds: none found in %s -- the menu will draw on its flat backdrop. ")
		       TEXT("Run ue-project/Tools/prepare_ui_assets.py to populate it."),
		       *Dir);
	}
	else
	{
		UE_LOG(LogVoxelUI, Log, TEXT("Menu backgrounds: %d found in %s."), Entries.Num(), *Dir);
	}
}

void FVoxelUIAssetLibrary::ShuffleOrder(FRandomStream& Stream)
{
	// Fisher-Yates over the index list, leaving Entries (and therefore every
	// decoded texture) untouched -- reshuffling must not throw away work
	// already paid for.
	for (int32 i = Order.Num() - 1; i > 0; --i)
	{
		const int32 j = Stream.RandRange(0, i);
		Order.Swap(i, j);
	}
}

const FSlateBrush* FVoxelUIAssetLibrary::RequestBackground(int32 Index)
{
	if (Order.Num() == 0)
	{
		return nullptr;
	}
	// Wraps, so callers can walk a rotation counter forward forever without
	// tracking the pool size.
	const int32 EntryIndex = Order[((Index % Order.Num()) + Order.Num()) % Order.Num()];
	FEntry& Entry = Entries[EntryIndex];

	if (Entry.Brush.IsValid())
	{
		return Entry.Brush.Get();
	}
	if (Entry.bDecodeFailed)
	{
		return nullptr;
	}
	if (!Entry.bDecodeStarted)
	{
		BeginDecode(EntryIndex);
	}
	// Still decoding. The caller draws nothing this frame and asks again --
	// which for a crossfade means the incoming image simply starts at zero
	// opacity for a few frames longer, and for the menu means one flat frame
	// before the art appears.
	return nullptr;
}

void FVoxelUIAssetLibrary::BeginDecode(int32 EntryIndex)
{
	Entries[EntryIndex].bDecodeStarted = true;
	const FString Path = Entries[EntryIndex].Path;

	// BackgroundNormal, the same priority the chunk mesher's jobs use. The
	// menu's art is emphatically not more urgent than the world it is hiding.
	UE::Tasks::Launch(UE_SOURCE_LOCATION,
	                  [this, EntryIndex, Path]()
	                  {
		                  TArray<uint8> Pixels;
		                  int32 Width = 0;
		                  int32 Height = 0;
		                  const bool bOk = VoxelUIAssetDetail::DecodeToBGRA(Path, Pixels, Width, Height);

		                  // The upload has to be on the game thread:
		                  // UTexture2D::CreateTransient makes a UObject, and
		                  // UpdateResource enqueues a render command.
		                  AsyncTask(ENamedThreads::GameThread,
		                            [this, EntryIndex, Path, bOk, Pixels = MoveTemp(Pixels), Width, Height]() mutable
		                            {
			                            if (!VoxelUIAssetDetail::GInstance)
			                            {
				                            return; // shut down while we were decoding
			                            }
			                            if (!bOk)
			                            {
				                            Entries[EntryIndex].bDecodeFailed = true;
				                            UE_LOG(LogVoxelUI, Warning,
				                                   TEXT("Menu background %s failed to decode; dropping it from the ")
				                                   TEXT("rotation. The others still rotate."),
				                                   *Path);
				                            return;
			                            }
			                            FinishDecode(EntryIndex, MoveTemp(Pixels), Width, Height);
		                            });
	                  },
	                  UE::Tasks::ETaskPriority::BackgroundNormal);
}

void FVoxelUIAssetLibrary::FinishDecode(int32 EntryIndex, TArray<uint8>&& Pixels, int32 Width, int32 Height)
{
	check(IsInGameThread());

	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	if (Texture == nullptr)
	{
		Entries[EntryIndex].bDecodeFailed = true;
		UE_LOG(LogVoxelUI, Warning, TEXT("Menu background %s: CreateTransient failed."), *Entries[EntryIndex].Path);
		return;
	}
	// SRGB on: these are ordinary display-referred photographs, and decoding
	// them as linear would wash the whole menu out.
	Texture->SRGB = true;
	// No mips and no compression: the image is drawn at roughly 1:1 as a
	// full-screen backdrop, so there is no minification to alias and nothing
	// for a mip chain to do but cost memory.
	Texture->MipGenSettings = TMGS_NoMipmaps;
	Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;

	void* MipData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(MipData, Pixels.GetData(), int64(Width) * int64(Height) * 4);
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();

	Cache->Textures.Add(Texture);

	TSharedRef<FSlateBrush> Brush = MakeShared<FSlateBrush>();
	Brush->SetResourceObject(Texture);
	Brush->ImageSize = FVector2D(float(Width), float(Height));
	Brush->DrawAs = ESlateBrushDrawType::Image;
	Entries[EntryIndex].Brush = Brush;

	UE_LOG(LogVoxelUI, Verbose, TEXT("Menu background %s decoded (%dx%d)."), *Entries[EntryIndex].Path, Width, Height);
}
