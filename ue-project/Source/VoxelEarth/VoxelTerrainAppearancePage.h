#pragma once
#include "VoxelAppearanceBankBinding.h"
#include "voxelcore/assetappearancepack.h"

// Per-call diagnostics only; never retained in the immutable page or shared
// across threads. Stage milliseconds are nested inside caller appearance time.
struct FVoxelAppearancePrepareStats {
    double FilterMs=0,ResourceMs=0,CanonicalMs=0,WinnerMs=0,PackMs=0,UploadMs=0;
    uint64 Input=0,Filtered=0,Cells=0,Words=0;
};

// Immutable representative-cell preparation for levels zero through seven. Caller supplies the COMPLETE ordered asset
// shortlist and immutable terrain/write-provenance callbacks for this generation.
// Empty Words means successful all-fallback appearance, never failed geometry.
// Publication must pair this snapshot/key/generation with the exact geometry page.
class FVoxelTerrainAppearancePage {
public:
    using FTerrain=TFunction<vxc::MaterialId(int64,int64,int64)>;
    using FTouched=TFunction<bool(int64,int64,int64)>;
    static TSharedPtr<const FVoxelTerrainAppearancePage,ESPMode::ThreadSafe> Prepare(
        FIntVector Key,uint32 Level,uint64 Generation,
        const std::vector<vxc::AssetField::ResolvedAssetInstance>& Ordered,
        const FVoxelAppearanceBankBinding& Binding,const FTerrain& Terrain,const FTouched& Touched,FString& Error,
        const vxc::AssetAppearanceTrace& Trace={},FVoxelAppearancePrepareStats* Stats=nullptr);
    TSharedPtr<const FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe> MakeUpload(const FVoxelAppearanceBankBinding& Binding,FString& Error,FVoxelAppearancePrepareStats* Stats=nullptr) const;
    const TArray<uint32>& Words() const {return Packed;}
    FIntVector PageKey() const {return Key;}
    uint32 PageLevel() const {return Level;}
    uint64 PageGeneration() const {return Generation;}
    TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> SourceSnapshot() const {return Catalog;}
private:
    FIntVector Key=FIntVector::ZeroValue;
    uint32 Level=0;
    uint64 Generation=0;
    TArray<uint32> Packed;
    TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> Catalog;
};
