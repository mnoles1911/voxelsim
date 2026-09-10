#pragma once
#include "VoxelPublishedAppearanceCatalog.h"
#include "VoxelTerrainAppearanceUpload.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "voxelcore/assetbank.h"

// Per-bank-library binding. Never infer appearance from seedIndex (the bank
// reduces it modulo accepted files). Grid addresses belong to this library.
class FVoxelAppearanceBankBinding {
public:
    explicit FVoxelAppearanceBankBinding(TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> InCatalog):Catalog(MoveTemp(InCatalog)){}
    void Observe(const vxc::AssetGrid& Grid,const uint8_t* Bytes,size_t Size) {
        // VXA admission already bounded the input. Hash exactly the bytes used
        // by the parser, never a second filesystem read of the same path.
        uint32 ID=0;
        if(Catalog&&Size<=uint64(MAX_int32))ID=Catalog->FindResource(FMD5::HashBytes(Bytes,int32(Size)));
        FScopeLock Lock(&Mutex);Resources.Add(&Grid,ID);
    }
    uint32 ResourceFor(const vxc::AssetGrid* Grid) const {
        FScopeLock Lock(&Mutex);const uint32* ID=Resources.Find(Grid);return ID?*ID:0;
    }
    TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> SourceSnapshot() const {return Catalog;}
    // Flatten once per binding snapshot, never per page. The upload owns all
    // source words so pool retirement need not know game-module catalog types.
    TSharedPtr<const FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe> UploadSources(FString& Error) const {
        Error.Reset();FScopeLock Lock(&Mutex);if(Upload)return Upload;
        if(!Catalog){Error=TEXT("missing appearance catalog");return nullptr;}
        uint64 Count=0;for(int32 I=1;I<Catalog->Sources().Num();++I){
            const auto& Source=Catalog->Sources()[I];
            if(!Source.Sparse){Error=TEXT("missing sparse source");return nullptr;}
            Count+=uint64(Source.Sparse->Words.Num());
        }
        // Explicit extra upload-copy budget, separate from retained VAC memory.
        const uint64 RawBytes=Count*4+uint64(Catalog->Sources().Num())*8;
        if(Count>uint64(MAX_int32)||RawBytes+RawBytes/2+512>256ull*1024*1024){Error=TEXT("flattened source upload budget exceeded");return nullptr;}
        auto Built=MakeShared<FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe>();
        Built->SourceWords.Reserve(int32(Count));Built->SourceRanges.SetNumZeroed(Catalog->Sources().Num());
        for(int32 I=1;I<Catalog->Sources().Num();++I){const auto& W=Catalog->Sources()[I].Sparse->Words;
            Built->SourceRanges[I]=FUintVector2(uint32(Built->SourceWords.Num()),uint32(W.Num()));Built->SourceWords.Append(W);
        }
        if(uint64(Built->SourceWords.GetAllocatedSize())+uint64(Built->SourceRanges.GetAllocatedSize())>256ull*1024*1024){Error=TEXT("source upload allocation exceeded budget");return nullptr;}
        Upload=Built;return Upload;
    }
private:
    TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> Catalog;
    mutable FCriticalSection Mutex;
    TMap<const vxc::AssetGrid*,uint32> Resources;
    mutable TSharedPtr<const FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe> Upload;
};
