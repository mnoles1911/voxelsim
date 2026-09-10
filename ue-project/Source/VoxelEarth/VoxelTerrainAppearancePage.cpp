#include "VoxelTerrainAppearancePage.h"
#include "VoxelFrameProfiling.h"
#include "Misc/ScopeExit.h"
namespace {
// Thread-local stack lifetime; no global counters or extra retained payload.
struct FAppearanceStageTimer {
    double& Ms;const char* Name;double Start=FPlatformTime::Seconds();
    ~FAppearanceStageTimer(){const double Elapsed=(FPlatformTime::Seconds()-Start)*1000.;Ms+=Elapsed;
#if CSV_PROFILER
        // Custom CSV names omit thread identity in UE. Keep this GT-hitch
        // series GT-only; worker Prepare timings remain in their local stats.
        if(IsInGameThread())FCsvProfiler::RecordCustomStat(Name,CSV_CATEGORY_INDEX(VoxelStream),Elapsed,ECsvCustomStatOp::Accumulate);
#endif
    }
};
}

TSharedPtr<const FVoxelTerrainAppearancePage,ESPMode::ThreadSafe> FVoxelTerrainAppearancePage::Prepare(
    FIntVector InKey,uint32 Level,uint64 InGeneration,
    const std::vector<vxc::AssetField::ResolvedAssetInstance>& AllOrdered,
    const FVoxelAppearanceBankBinding& Binding,const FTerrain& Terrain,const FTouched& Touched,FString& Error,const vxc::AssetAppearanceTrace& Trace,FVoxelAppearancePrepareStats* OutStats)
{
    FVoxelAppearancePrepareStats Stats;Stats.Input=AllOrdered.size();
    ON_SCOPE_EXIT {
        if(OutStats)*OutStats=Stats;
        if(IsInGameThread()){
        CSV_CUSTOM_STAT(VoxelStream,AppearanceInputCandidates,double(Stats.Input),ECsvCustomStatOp::Accumulate);
        CSV_CUSTOM_STAT(VoxelStream,AppearanceFilteredCandidates,double(Stats.Filtered),ECsvCustomStatOp::Accumulate);
        CSV_CUSTOM_STAT(VoxelStream,AppearanceCells,double(Stats.Cells),ECsvCustomStatOp::Accumulate);
        CSV_CUSTOM_STAT(VoxelStream,AppearancePackedWords,double(Stats.Words),ECsvCustomStatOp::Accumulate);
#if CSV_PROFILER
        static const char* LevelNames[]={"AppearanceL0Calls","AppearanceL1Calls","AppearanceL2Calls","AppearanceL3Calls","AppearanceL4Calls","AppearanceL5Calls","AppearanceL6Calls","AppearanceL7Calls"};
        if(Level<8)FCsvProfiler::RecordCustomStat(LevelNames[Level],CSV_CATEGORY_INDEX(VoxelStream),1,ECsvCustomStatOp::Accumulate);
#endif
        } // GT-only CSV; OutStats above remains available on every thread.
    };
    Error.Reset();auto Fail=[&](const TCHAR* Why)->TSharedPtr<const FVoxelTerrainAppearancePage,ESPMode::ThreadSafe>{Error=Why;return nullptr;};
    if(Level>7||!InGeneration||!Terrain||!Touched)return Fail(TEXT("invalid appearance preparation level or inputs"));
    // Broad XY footprints may include thousands of trees at unrelated heights
    // or between coarse representatives. Remove only impossible contributors;
    // preserve the full order of approved AND unapproved potential winners.
    const int64 Scale=int64(1)<<Level,Half=Level?Scale/2:0;
    const int64 OX=int64(InKey.X)*32*Scale+Half,OY=int64(InKey.Y)*32*Scale+Half,OZ=int64(InKey.Z)*32*Scale+Half;
    auto Hits=[&](int64 Low,int64 High,int64 Origin){
        if(Trace)return Low<=Origin-Half+32*Scale-1&&High>=Origin-Half;
        const int64 L=FMath::Max(Low,Origin),H=FMath::Min(High,Origin+31*Scale);
        if(L>H)return false;
        return Origin+((L-Origin+Scale-1)/Scale)*Scale<=H;
    };
    std::vector<vxc::AssetField::ResolvedAssetInstance> Ordered;
    Ordered.reserve(FMath::Min<size_t>(AllOrdered.size(),4096));
    { FAppearanceStageTimer Timer{Stats.FilterMs,"AppearanceFilterMs"};
    for(const auto& Instance:AllOrdered){
        vxc::AssetCandidateBounds B;if(!vxc::assetCandidateBounds(Instance,B))return Fail(TEXT("invalid 100mm source instance"));
        if(!Hits(B.x0,B.x1,OX)||!Hits(B.y0,B.y1,OY)||!Hits(B.z0,B.z1,OZ))continue;
        if(Ordered.size()==4096)return Fail(TEXT("appearance page exceeds 4096 intersecting source instances"));
        Ordered.push_back(Instance);
    }
    }
    Stats.Filtered=Ordered.size();
    auto Snapshot=Binding.SourceSnapshot();std::vector<uint32_t> Resources;Resources.reserve(Ordered.size());
    { FAppearanceStageTimer Timer{Stats.ResourceMs,"AppearanceResourceMs"};
    for(const auto& Instance:Ordered){
        vxc::AssetCandidateBounds Bounds;
        if(!vxc::assetCandidateBounds(Instance,Bounds))return Fail(TEXT("invalid 100mm source instance"));
        const uint32 ID=Binding.ResourceFor(Instance.grid);Resources.push_back(ID);
        if(!ID)continue;
        if(!Snapshot||ID>=uint32(Snapshot->Sources().Num()))return Fail(TEXT("resource outside retained catalog"));
        const auto& Source=Snapshot->Sources()[ID];
        if(!Source.Appearance||!Source.Sparse||Source.Appearance->PitchMm()!=100||Snapshot->FindResourceId(Source.GeometryMD5)!=ID)return Fail(TEXT("invalid approved source identity or pitch"));
        const auto& W=Source.Sparse->Words;const auto& G=*Instance.grid;
        if(W.Num()<20||(W[0]!=1&&W[0]!=2)||W[1]!=uint32(W.Num())||W[13]!=(W[0]==1?100u:100000u)||W[7]!=uint32(G.sizeX())||W[8]!=uint32(G.sizeY())||W[9]!=uint32(G.sizeZ())||
           int32(W[10])!=G.originX()||int32(W[11])!=G.originY()||int32(W[12])!=G.originZ()||W[2]!=G.solidCount())return Fail(TEXT("bound grid differs from approved source layout"));
        uint8 Hash[16];for(int I=0;I<16;++I)Hash[I]=uint8(W[16+I/4]>>(8*(I%4)));
        if(!BytesToHex(Hash,16).Equals(Source.GeometryMD5,ESearchCase::IgnoreCase))return Fail(TEXT("sparse source identity mismatch"));
    }
    }
    std::vector<vxc::AssetAppearanceCell> Cells;
    { FAppearanceStageTimer Timer{Stats.CanonicalMs,"AppearanceCanonicalMs"};
    if(!vxc::assetAppearancePage(InKey.X,InKey.Y,InKey.Z,Ordered,Resources,Terrain,Touched,Cells,uint8(Level),Trace))return Fail(TEXT("canonical appearance generation failed"));
    }
    Stats.Cells=Cells.size();
    // Verify each emitted winner against the retained original VAC resource.
    { FAppearanceStageTimer Timer{Stats.WinnerMs,"AppearanceWinnerMs"};
    for(const auto& Cell:Cells){FColor Color;FIntVector Zero;
        if(!Snapshot->Sources()[Cell.resource].Sparse->Lookup(FIntVector(Cell.sourceX,Cell.sourceY,Cell.sourceZ),uint8(Cell.material),Color,Zero))return Fail(TEXT("canonical winner does not match approved source"));
    }
    }
    std::vector<uint32_t> Words;
    { FAppearanceStageTimer Timer{Stats.PackMs,"AppearancePackMs"};
    if(!vxc::assetPackAppearancePage(InKey.X,InKey.Y,InKey.Z,InGeneration,Ordered,Cells,Words,uint8(Level)))return Fail(TEXT("appearance page packing failed"));
    }
    Stats.Words=Words.size();
    auto Result=MakeShared<FVoxelTerrainAppearancePage,ESPMode::ThreadSafe>();Result->Key=InKey;Result->Level=Level;Result->Generation=InGeneration;Result->Catalog=MoveTemp(Snapshot);
    if(!Words.empty())Result->Packed.Append(Words.data(),int32(Words.size()));
    return Result;
}

TSharedPtr<const FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe> FVoxelTerrainAppearancePage::MakeUpload(const FVoxelAppearanceBankBinding& Binding,FString& Error,FVoxelAppearancePrepareStats* Stats) const {
    double Ignored=0;FAppearanceStageTimer Timer{Stats?Stats->UploadMs:Ignored,"AppearanceUploadMs"};
    Error.Reset();if(Binding.SourceSnapshot()!=Catalog){Error=TEXT("appearance upload snapshot mismatch");return nullptr;}
    auto Sources=Binding.UploadSources(Error);if(!Sources)return nullptr;
    auto Upload=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>();Upload->PageKey=Key;Upload->Level=Level;Upload->Generation=Generation;Upload->PageWords=Packed;Upload->Sources=MoveTemp(Sources);return Upload;
}
