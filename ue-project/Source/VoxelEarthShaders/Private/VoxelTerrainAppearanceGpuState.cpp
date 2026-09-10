#include "VoxelTerrainAppearanceGpuState.h"
#include "VoxelTerrainAppearanceValidation.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
namespace {
// Expanded temperate field exceeded the former 64 MiB page allocation. Keep
// a finite 128 MiB resident-page budget and report utilization during runs.
constexpr uint32 PageWords=32u*1024*1024,SourceWords=64u*1024*1024,RangeCount=1024u*1024,SlotCount=1024u*1024;
bool ValidUpload(const FVoxelTerrainAppearanceUpload& U){
    const auto& P=U.PageWords;
    const bool Recursive=P.Num()>=80&&P[0]==3;
    if(!U.Sources||P.Num()<80||P.Num()>(Recursive?98304:65536)||U.Level>7||
       (Recursive?!VoxelValidateRecursiveAppearancePage(U):(P[0]!=(U.Level?2u:1u)||P[15]!=0))||P[1]!=uint32(P.Num())||P[5]!=U.Level||!U.Generation||
       int32(P[2])!=U.PageKey.X||int32(P[3])!=U.PageKey.Y||int32(P[4])!=U.PageKey.Z||P[13]!=uint32(U.Generation)||P[14]!=uint32(U.Generation>>32))return false;
    const auto& Input=*U.Sources;
    if(Input.SourceWords.IsEmpty()||uint64(Input.SourceWords.Num())>SourceWords||Input.SourceRanges.IsEmpty()||uint64(Input.SourceRanges.Num())>RangeCount||Input.SourceRanges[0]!=FUintVector2(0,0))return false;
    for(const auto& Range:Input.SourceRanges)if(uint64(Range.X)+Range.Y>uint64(Input.SourceWords.Num()))return false;
    return true;
}
struct FSpan {uint32 Base=0,Count=0;};
struct FArena {
    TArray<FSpan> Free;
    void Init(uint32 Count){Free={{0,Count}};}
    uint32 Available()const{uint32 Total=0;for(const auto& S:Free)Total+=S.Count;return Total;}
    uint32 Largest()const{uint32 Result=0;for(const auto& S:Free)Result=FMath::Max(Result,S.Count);return Result;}
    bool Take(uint32 Count,FSpan& Out){if(!Count)return false;for(int32 I=0;I<Free.Num();++I)if(Free[I].Count>=Count){Out={Free[I].Base,Count};Free[I].Base+=Count;Free[I].Count-=Count;if(!Free[I].Count)Free.RemoveAt(I);return true;}return false;}
    void Put(FSpan S){if(!S.Count)return;Free.Add(S);Free.Sort([](const FSpan& A,const FSpan& B){return A.Base<B.Base;});for(int32 I=1;I<Free.Num();)if(uint64(Free[I-1].Base)+Free[I-1].Count==Free[I].Base){Free[I-1].Count+=Free[I].Count;Free.RemoveAt(I);}else ++I;}
};
struct FBuffer {
    FBufferRHIRef RHI;FShaderResourceViewRHIRef View;TRefCountPtr<FRDGPooledBuffer> Pooled;
    bool Init(FRHICommandListImmediate& Cmd,const TCHAR* Name,uint32 Stride,uint32 Count){
        const auto Desc=FRHIBufferCreateDesc::CreateStructured(Name,uint64(Stride)*Count,Stride).AddUsage(EBufferUsageFlags::Static|EBufferUsageFlags::ShaderResource|EBufferUsageFlags::UnorderedAccess).DetermineInitialState().SetInitActionZeroData();
        RHI=Cmd.CreateBuffer(Desc);if(!RHI.IsValid())return false;
        View=Cmd.CreateShaderResourceView(RHI,FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(RHI));if(!View.IsValid())return false;
        Pooled=new FRDGPooledBuffer(Cmd,RHI,FRDGBufferDesc::CreateStructuredDesc(Stride,Count),Count,Name);return true;
    }
    bool Write(FRHICommandListImmediate& Cmd,uint32 ByteOffset,const void* Data,uint32 Bytes){if(!Bytes)return true;void* Dest=Cmd.LockBuffer(RHI,ByteOffset,Bytes,RLM_WriteOnly);if(!Dest)return false;FMemory::Memcpy(Dest,Data,Bytes);Cmd.UnlockBuffer(RHI);return true;}
};
FRDGBufferRef Dummy(FRDGBuilder& Graph,const TCHAR* Name,uint32 Stride){auto B=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(Stride,1),Name);AddClearUAVPass(Graph,Graph.CreateUAV(B),0u);return B;}
}
struct FVoxelTerrainAppearanceGpuState::FImpl {
    struct FSource {TSharedPtr<const FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe> Snapshot;FSpan Words,Ranges;uint32 References=0;};
    struct FPage {FSpan Words;const FVoxelTerrainAppearanceSources* Source=nullptr;};
    FBuffer Pages,Sources,Ranges,Slots;
    FBuffer EmptyPages,EmptySources,EmptyRanges,EmptySlots;
    bool EmptyInitialized=false;
    FArena PageArena,SourceArena,RangeArena;
    TMap<const FVoxelTerrainAppearanceSources*,FSource> SourceMap;TMap<uint32,FPage> PageMap;
    bool Initialized=false,Poisoned=false;
    uint32 ReportedPageBucket=0;
    bool Init(FRHICommandListImmediate& RHI){
        if(Initialized)return true;
        if(!Pages.Init(RHI,TEXT("VoxelAppearance.Pages"),4,PageWords)||!Sources.Init(RHI,TEXT("VoxelAppearance.Sources"),4,SourceWords)||
           !Ranges.Init(RHI,TEXT("VoxelAppearance.Ranges"),8,RangeCount)||!Slots.Init(RHI,TEXT("VoxelAppearance.Slots"),16,SlotCount)){Poisoned=true;return false;}
        PageArena.Init(PageWords);SourceArena.Init(SourceWords);RangeArena.Init(RangeCount);Initialized=true;return true;
    }
    bool Clear(FRHICommandListImmediate& RHI,uint32 Slot){
        const FUintVector4 Zero(0,0,0,0);if(!Slots.Write(RHI,Slot*16,&Zero,16)){Poisoned=true;return false;}
        FPage Old;if(PageMap.RemoveAndCopyValue(Slot,Old)){
            PageArena.Put(Old.Words);auto* Source=SourceMap.Find(Old.Source);
            if(Source && --Source->References==0){SourceArena.Put(Source->Words);RangeArena.Put(Source->Ranges);SourceMap.Remove(Old.Source);}
        }return true;
    }
};
FVoxelTerrainAppearanceGpuState::FVoxelTerrainAppearanceGpuState():Impl(MakeUnique<FImpl>()){}
FVoxelTerrainAppearanceGpuState::~FVoxelTerrainAppearanceGpuState()=default;
void FVoxelTerrainAppearanceGpuState::Reset(){check(IsInRenderingThread());Impl=MakeUnique<FImpl>();}
bool FVoxelTerrainAppearanceGpuState::ApplyBatch(FRHICommandListImmediate& RHI,uint32 ChunkCapacity,TConstArrayView<uint32> ClearSlots,TConstArrayView<FEntry> Entries,FString& Error){
    check(IsInRenderingThread());Error.Reset();auto& S=*Impl;
    auto Fail=[&](const TCHAR* Why){if(Error.IsEmpty())Error=Why;};
    if(S.Poisoned){Error=TEXT("appearance GPU state requires reset after buffer failure");return false;}
    if(!ChunkCapacity||ChunkCapacity>SlotCount){S.Poisoned=true;Error=TEXT("appearance chunk capacity exceeds one million slots");return false;}
    bool HasData=false;
    for(uint32 Slot:ClearSlots)if(Slot>=SlotCount)Fail(TEXT("invalid appearance clear slot"));
    for(const auto& E:Entries){
        if(E.Slot>=ChunkCapacity){Fail(TEXT("appearance entry outside chunk capacity"));continue;}
        if(E.Upload&&!E.Upload->PageWords.IsEmpty()){
            if(ValidUpload(*E.Upload))HasData=true;else Fail(TEXT("invalid appearance upload admission"));
        }
    }
    if(!S.Initialized&&!HasData)return Error.IsEmpty();
    if(!S.Init(RHI)){Error=TEXT("appearance persistent buffer allocation failed");return false;}
    for(uint32 Slot:ClearSlots){if(Slot>=SlotCount){Fail(TEXT("invalid appearance clear slot"));continue;}if(!S.Clear(RHI,Slot)){Fail(TEXT("appearance descriptor clear failed"));break;}}
    for(const auto& E:Entries){
        if(S.Poisoned)break;
        if(E.Slot>=ChunkCapacity){if(E.Slot<SlotCount)S.Clear(RHI,E.Slot);Fail(TEXT("appearance entry outside chunk capacity"));continue;}
        if(!S.Clear(RHI,E.Slot)){Fail(TEXT("appearance descriptor clear failed"));break;}
        if(!E.Upload||E.Upload->PageWords.IsEmpty())continue;
        const auto& U=*E.Upload;const auto& P=U.PageWords;
        if(!ValidUpload(U)){Fail(TEXT("invalid appearance page upload"));continue;}
        const auto* Identity=U.Sources.Get();auto* Source=S.SourceMap.Find(Identity);bool NewSource=false;
        if(!Source){
            const auto& Input=*U.Sources;bool Valid=Input.SourceRanges.Num()>0&&Input.SourceRanges[0]==FUintVector2(0,0);
            for(const auto& Range:Input.SourceRanges)if(uint64(Range.X)+Range.Y>uint64(Input.SourceWords.Num()))Valid=false;
            if(!Valid||Input.SourceWords.IsEmpty()){Fail(TEXT("invalid appearance source ranges"));continue;}
            FImpl::FSource Built;Built.Snapshot=U.Sources;
            if(!S.SourceArena.Take(uint32(Input.SourceWords.Num()),Built.Words)){Fail(TEXT("appearance source arena exhausted"));continue;}
            if(!S.RangeArena.Take(uint32(Input.SourceRanges.Num()),Built.Ranges)){S.SourceArena.Put(Built.Words);Fail(TEXT("appearance range arena exhausted"));continue;}
            TArray<FUintVector2> Relocated;Relocated.Reserve(Input.SourceRanges.Num());
            for(const auto& Range:Input.SourceRanges)Relocated.Add(FUintVector2(Built.Words.Base+Range.X,Range.Y));
            if(!S.Sources.Write(RHI,Built.Words.Base*4,Input.SourceWords.GetData(),uint32(Input.SourceWords.Num())*4)||
               !S.Ranges.Write(RHI,Built.Ranges.Base*8,Relocated.GetData(),uint32(Relocated.Num())*8)){
                S.SourceArena.Put(Built.Words);S.RangeArena.Put(Built.Ranges);S.Poisoned=true;Fail(TEXT("appearance source upload failed"));break;
            }
            S.SourceMap.Add(Identity,MoveTemp(Built));Source=S.SourceMap.Find(Identity);NewSource=true;
        }
        FSpan Page;
        if(!S.PageArena.Take(uint32(P.Num()),Page)){
            if(NewSource){S.SourceArena.Put(Source->Words);S.RangeArena.Put(Source->Ranges);S.SourceMap.Remove(Identity);}
            if(Error.IsEmpty())Error=FString::Printf(TEXT("appearance page arena exhausted: requested=%u free=%u largest=%u capacity=%u words, residentPages=%d"),
                uint32(P.Num()),S.PageArena.Available(),S.PageArena.Largest(),PageWords,S.PageMap.Num());
            continue;
        }
        const FUintVector4 Descriptor(Page.Base,Page.Count,Source->Ranges.Base,Source->Ranges.Count);
        if(!S.Pages.Write(RHI,Page.Base*4,P.GetData(),uint32(P.Num())*4)||!S.Slots.Write(RHI,E.Slot*16,&Descriptor,16)){
            S.PageArena.Put(Page);S.Poisoned=true;Fail(TEXT("appearance page publication failed"));break;
        }
        ++Source->References;S.PageMap.Add(E.Slot,{Page,Identity});
    }
    const uint32 Used=PageWords-S.PageArena.Available(),Bucket=Used/(PageWords/8);
    if(Bucket>S.ReportedPageBucket){S.ReportedPageBucket=Bucket;
        UE_LOG(LogTemp,Display,TEXT("TerrainAppearance residency: pageBytes=%llu capacityBytes=%llu residentPages=%d sourceBytes=%llu"),
            uint64(Used)*4,uint64(PageWords)*4,S.PageMap.Num(),uint64(SourceWords-S.SourceArena.Available())*4);}
    return Error.IsEmpty()&&!S.Poisoned;
}
FVoxelTerrainAppearanceGpuState::FBuffers FVoxelTerrainAppearanceGpuState::Register(FRDGBuilder& Graph){
    check(IsInRenderingThread());auto& S=*Impl;
    if(!S.Initialized||S.Poisoned)return {Dummy(Graph,TEXT("VoxelAppearance.EmptyPages"),4),Dummy(Graph,TEXT("VoxelAppearance.EmptySources"),4),Dummy(Graph,TEXT("VoxelAppearance.EmptyRanges"),8),Dummy(Graph,TEXT("VoxelAppearance.EmptySlots"),16)};
    return {Graph.RegisterExternalBuffer(S.Pages.Pooled),Graph.RegisterExternalBuffer(S.Sources.Pooled),Graph.RegisterExternalBuffer(S.Ranges.Pooled),Graph.RegisterExternalBuffer(S.Slots.Pooled)};
}

FVoxelTerrainAppearanceGpuState::FViews FVoxelTerrainAppearanceGpuState::GetViews(FRHICommandListImmediate& RHI){
    check(IsInRenderingThread());auto& S=*Impl;
    if(S.Initialized&&!S.Poisoned)return {S.Pages.View,S.Sources.View,S.Ranges.View,S.Slots.View};
    if(!S.EmptyInitialized){
        const bool OK=S.EmptyPages.Init(RHI,TEXT("VoxelAppearance.RasterEmptyPages"),4,1)&&
            S.EmptySources.Init(RHI,TEXT("VoxelAppearance.RasterEmptySources"),4,1)&&
            S.EmptyRanges.Init(RHI,TEXT("VoxelAppearance.RasterEmptyRanges"),8,1)&&
            S.EmptySlots.Init(RHI,TEXT("VoxelAppearance.RasterEmptySlots"),16,1);
        if(!OK)return {};
        S.EmptyInitialized=true;
    }
    return {S.EmptyPages.View,S.EmptySources.View,S.EmptyRanges.View,S.EmptySlots.View};
}
