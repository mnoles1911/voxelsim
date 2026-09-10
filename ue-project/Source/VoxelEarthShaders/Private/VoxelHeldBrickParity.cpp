#include "VoxelHeldBrickParity.h"
#include "Containers/Ticker.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "voxelcore/brickpack.h"
#include <atomic>
#include <bit>

namespace HeldParityPrivate
{
bool DecodeChecked(const FVoxelBrickCpuPack& In, uint32 OccBase, uint32 MatBase,
    vxc::ChunkBrickPack& Out, FString& Error)
{
    if (In.Desc.Num()!=128 || In.Occ.Num()>1024 || In.Mat.Num()>8448)
    { Error=TEXT("Invalid brick array bounds"); return false; }
    if (!In.Occ.IsEmpty()) Out.occ.assign(In.Occ.GetData(),In.Occ.GetData()+In.Occ.Num());
    if (!In.Mat.IsEmpty()) Out.mat.assign(In.Mat.GetData(),In.Mat.GetData()+In.Mat.Num());
    uint64 Mask=0;
    for (uint32 I=0; I<64; ++I)
    {
        auto& D=Out.descs[I]; D.OccWord=In.Desc[I*2]; D.MatWord=In.Desc[I*2+1];
        const uint32 Kind=D.kind(), Bpp=D.bppCode();
        if ((D.OccWord&0x80000000u)!=0 || Kind==3)
        { Error=TEXT("Invalid brick descriptor kind/reserved flag"); return false; }
        if (Kind==0) continue;
        Mask|=uint64(1)<<I;
        if (Kind==1)
        {
            if (D.hasLocalPalette() || D.uniformMaterial()==0 || (D.MatWord&0xffffff00u)!=0)
            { Error=TEXT("Invalid uniform brick descriptor"); return false; }
            continue;
        }
        if (!((Bpp==0 || Bpp==1 || Bpp==2 || Bpp==4) && D.hasLocalPalette()) &&
            !(Bpp==8 && !D.hasLocalPalette()))
        { Error=TEXT("Invalid mixed brick material mode"); return false; }
        const uint32 O=D.occDwordOffset(), M=D.matDwordOffset();
        if (O<OccBase || M<MatBase || uint64(O-OccBase)+16>uint64(In.Occ.Num()))
        { Error=TEXT("Invalid mixed brick occupancy offset"); return false; }
        uint32 Solids=0;
        for (uint32 J=0;J<16;++J) Solids+=std::popcount(In.Occ[O-OccBase+J]);
        const uint64 Words=(D.hasLocalPalette()?4ull:0ull)+(uint64(Solids)*Bpp+31)/32;
        if (uint64(M-MatBase)+Words>uint64(In.Mat.Num()))
        { Error=TEXT("Invalid mixed brick material offset"); return false; }
        D.OccWord=(D.OccWord&0xf0000000u)|(O-OccBase);
        D.MatWord=(D.MatWord&0xf0000000u)|(M-MatBase);
    }
    if (Mask!=In.BrickSolid) { Error=TEXT("Brick occupancy mask disagrees with descriptors"); return false; }
    Out.brickSolid=Mask;
    return true;
}
}

bool FVoxelHeldBrickParity::ComparePacks(const FVoxelBrickCpuPack& Cpu,const FVoxelBrickCpuPack& Gpu,
    uint32 GpuOccBase,uint32 GpuMatBase,FVoxelHeldBrickParityResult& Out)
{
    Out.Mismatches=0; Out.FirstMismatch=FIntVector(-1); Out.Error.Reset();
    vxc::ChunkBrickPack A,B;
    if (Cpu.OriginVoxel!=Gpu.OriginVoxel) { Out.Error=TEXT("Brick origin mismatch"); return false; }
    if (!HeldParityPrivate::DecodeChecked(Cpu,0,0,A,Out.Error) ||
        !HeldParityPrivate::DecodeChecked(Gpu,GpuOccBase,GpuMatBase,B,Out.Error)) return false;
    for (int32 Z=0;Z<32;++Z) for (int32 Y=0;Y<32;++Y) for (int32 X=0;X<32;++X)
    {
        const uint8 C=vxc::decodeChunkVoxelCanonical(A,X,Y,Z), G=vxc::decodeChunkVoxelCanonical(B,X,Y,Z);
        if (C!=G)
        {
            if (Out.Mismatches++==0) { Out.FirstMismatch=FIntVector(X,Y,Z); Out.CpuMaterial=C; Out.GpuMaterial=G; }
        }
    }
    if (Out.Mismatches) { Out.Error=TEXT("CPU/GPU brick material mismatch"); return false; }
    return true;
}

struct FVoxelHeldBrickParity::FState
{
    std::atomic<bool> Done{false}, Cancelled{false}, PollQueued{false};
    EVoxelHeldBrickParityStatus Status=EVoxelHeldBrickParityStatus::Pending;
    FVoxelHeldBrickParityResult Result;
    FVoxelBrickCpuPack Cpu;
    FVoxelGpuBrickPayloadRef Gpu;
    TUniquePtr<FRHIGPUBufferReadback> Reads[4]; // RT only, explicitly retired before Done.
    uint32 Bytes[4]{};
    bool Issued=false;
    void Finish(EVoxelHeldBrickParityStatus NewStatus)
    {
        check(IsInRenderingThread());
        for (auto& R:Reads) R.Reset();
        Gpu.Reset(); Cpu.Desc.Empty(); Cpu.Occ.Empty(); Cpu.Mat.Empty();
        Status=NewStatus; Done.store(true,std::memory_order_release);
    }
    void ReadReady()
    {
        check(IsInRenderingThread());
        if (Done.load(std::memory_order_acquire)) return;
        if (!Issued) return;
        for (auto& R:Reads) if (R && !R->IsReady()) return;
        if (Cancelled.load()) { Finish(EVoxelHeldBrickParityStatus::Cancelled); return; }
        FVoxelBrickCpuPack Pack; Pack.OriginVoxel=Gpu->OriginVoxel;
        TArray<uint32>* Arrays[]={&Pack.Desc,&Pack.Occ,&Pack.Mat};
        for (int32 I=0;I<4;++I)
        {
            if (!Bytes[I]) continue;
            const void* Data=Reads[I]->Lock(Bytes[I]);
            if (!Data) { Pack.Desc.Empty(); Pack.Occ.Empty(); Pack.Mat.Empty(); Result.Error=TEXT("GPU brick readback lock failed"); Finish(EVoxelHeldBrickParityStatus::Failed); return; }
            if (I<3) { Arrays[I]->SetNumUninitialized(Bytes[I]/4); FMemory::Memcpy(Arrays[I]->GetData(),Data,Bytes[I]); }
            else FMemory::Memcpy(&Pack.BrickSolid,Data,8);
            Reads[I]->Unlock();
        }
        const bool Ok=ComparePacks(Cpu,Pack,Gpu->SrcOccFirst,Gpu->SrcMatFirst,Result);
        Pack.Desc.Empty(); Pack.Occ.Empty(); Pack.Mat.Empty();
        Finish(Ok?EVoxelHeldBrickParityStatus::Passed:EVoxelHeldBrickParityStatus::Failed);
    }
};
TSharedPtr<FVoxelHeldBrickParity::FState,ESPMode::ThreadSafe> FVoxelHeldBrickParity::Active;

bool FVoxelHeldBrickParity::Begin(const FVoxelBrickCpuPackRef& Cpu,const FVoxelGpuBrickPayloadRef& Gpu,
    const FIntVector& ExpectedOrigin,uint64 Generation,FString& Error)
{
    check(IsInGameThread()); Error.Reset();
    if (Active && !Active->Done.load(std::memory_order_acquire))
    { Error=TEXT("A private brick readback is still active or retiring"); return false; }
    if (!Cpu || !Gpu || Cpu->OriginVoxel!=ExpectedOrigin || Gpu->OriginVoxel!=ExpectedOrigin ||
        Gpu->BrickCount!=64 || Gpu->OccWords>1024 || Gpu->MatWords>8448)
    { Error=TEXT("Invalid private brick payload shape/origin"); return false; }
    vxc::ChunkBrickPack Validated;
    if (!HeldParityPrivate::DecodeChecked(*Cpu,0,0,Validated,Error)) return false;
    auto S=MakeShared<FState,ESPMode::ThreadSafe>(); S->Cpu=*Cpu; S->Gpu=Gpu;
    S->Result.OwnershipGeneration=Generation; State=S; Active=S;
    ENQUEUE_RENDER_COMMAND(BeginHeldBrickReadback)([S](FRHICommandListImmediate& RHICmdList)
    {
        if (S->Cancelled.load()) { S->Finish(EVoxelHeldBrickParityStatus::Cancelled); return; }
        const auto& G=*S->Gpu;
        TRefCountPtr<FRDGPooledBuffer> Buffers[]={G.Desc,G.Occ,G.Mat,G.ChunkMask};
        const uint64 Offsets[]={uint64(G.SrcBrickFirst)*8,uint64(G.SrcOccFirst)*4,uint64(G.SrcMatFirst)*4,uint64(G.SrcChunkIndex)*8};
        const uint32 Sizes[]={512,G.OccWords*4,G.MatWords*4,8};
        for (int32 I=0;I<4;++I) if (Sizes[I] && (!Buffers[I] || Offsets[I]+Sizes[I]>Buffers[I]->GetSize()))
        { S->Result.Error=TEXT("GPU brick source range exceeds buffer capacity"); S->Finish(EVoxelHeldBrickParityStatus::Failed); return; }
        FRDGBuilder Graph(RHICmdList);
        for (int32 I=0;I<4;++I)
        {
            S->Bytes[I]=Sizes[I]; if (!Sizes[I]) continue;
            S->Reads[I]=MakeUnique<FRHIGPUBufferReadback>(TEXT("HeldBrickParity"));
            FRDGBufferRef Source=Graph.RegisterExternalBuffer(Buffers[I]);
            FRDGBufferRef Slice=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(4,Sizes[I]/4),TEXT("HeldBrickParitySlice"));
            AddCopyBufferPass(Graph,Slice,0,Source,Offsets[I],Sizes[I]);
            AddEnqueueCopyPass(Graph,S->Reads[I].Get(),Slice,Sizes[I]);
        }
        Graph.Execute(); S->Issued=true;
    });
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([S](float)
    {
        if (S->Done.load(std::memory_order_acquire)) return false;
        if (!S->PollQueued.exchange(true))
        {
            ENQUEUE_RENDER_COMMAND(PollHeldBrickReadback)([S](FRHICommandListImmediate&)
            { S->ReadReady(); S->PollQueued.store(false); });
        }
        return true;
    }));
    return true;
}
EVoxelHeldBrickParityStatus FVoxelHeldBrickParity::Poll(FVoxelHeldBrickParityResult& Out) const
{
    check(IsInGameThread());
    if (!State) { Out={}; Out.Error=TEXT("No private brick comparison started"); return EVoxelHeldBrickParityStatus::Failed; }
    if (!State->Done.load(std::memory_order_acquire)) return EVoxelHeldBrickParityStatus::Pending;
    Out=State->Result; return State->Status;
}
void FVoxelHeldBrickParity::Cancel() { check(IsInGameThread()); if (State) State->Cancelled.store(true); }
FVoxelHeldBrickParity::~FVoxelHeldBrickParity() { Cancel(); }
bool FVoxelHeldBrickParity::IsRetirementPending()
{ check(IsInGameThread()); return Active && !Active->Done.load(std::memory_order_acquire); }
void FVoxelHeldBrickParity::DrainForShutdown()
{
    check(IsInGameThread()); const auto S=Active;
    if (!S || S->Done.load(std::memory_order_acquire)) return;
    S->Cancelled.store(true);
    ENQUEUE_RENDER_COMMAND(DrainHeldBrickReadbackForShutdown)([S](FRHICommandListImmediate& RHICmdList)
    {
        if (!S->Done.load(std::memory_order_acquire))
        {
            RHICmdList.SubmitAndBlockUntilGPUIdle();
            S->Finish(EVoxelHeldBrickParityStatus::Cancelled);
        }
    });
}
