#include "VoxelAssetAppearance.h"
#include "voxelcore/core.h"
#include "HAL/PlatformFileManager.h"
#include <openssl/sha.h>
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "VoxelEarth.h"

namespace {
constexpr int32 HeaderBytes=128,RecordBytes=10,MaxBytes=256*1024*1024;
uint16 U16(const uint8* P){return uint16(P[0])|(uint16(P[1])<<8);}
uint32 U32(const uint8* P){return uint32(P[0])|(uint32(P[1])<<8)|(uint32(P[2])<<16)|(uint32(P[3])<<24);}
FIntVector XYZ(const uint8* P){return FIntVector(U16(P),U16(P+2),U16(P+4));}
bool ValidHash(const FString& S){if(S.Len()!=32)return false;for(TCHAR C:S)if(!FChar::IsHexDigit(C))return false;return true;}
uint32 ColorHash(uint32 H){H^=H>>16;H*=2246822519u;H^=H>>13;H*=3266489917u;return H^(H>>16);}
float Unit(uint32 H){return float(ColorHash(H)&65535u)/65535.f*2.f-1.f;}
}

TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> FVoxelAssetAppearance::Parse(TArray<uint8> Bytes,const FString& SourceMD5,FString& Error)
{
    Error.Reset();
    auto Fail=[&](const TCHAR* Why)->TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe>{Error=Why;return nullptr;};
    if(!ValidHash(SourceMD5)||Bytes.Num()<HeaderBytes||Bytes.Num()>MaxBytes)return Fail(TEXT("invalid size or source identity"));
    const uint8* P=Bytes.GetData();
    const uint32 Version=U32(P+4),Flags=U32(P+44);
    if(FMemory::Memcmp(P,"VAC1",4)||(Version!=1&&Version!=2)||(Version==1?Flags!=0:Flags>1)||U32(P+40)>1)return Fail(TEXT("unsupported appearance header"));
    if(!BytesToHex(P+48,16).Equals(SourceMD5,ESearchCase::IgnoreCase))return Fail(TEXT("source geometry identity mismatch"));
    const uint32 N=U32(P+36),Pitch=U32(P+32);
    if(!N||int64(HeaderBytes)+int64(N)*RecordBytes!=Bytes.Num()||(Version==1?(Pitch!=25&&Pitch!=50&&Pitch!=100):(Pitch<1||Pitch>1000000)))return Fail(TEXT("invalid appearance count or pitch"));
    const FIntVector S(int32(U32(P+8)),int32(U32(P+12)),int32(U32(P+16)));
    const FIntVector O(int32(U32(P+20)),int32(U32(P+24)),int32(U32(P+28)));
    if(S.GetMin()<=0||S.GetMax()>65535)return Fail(TEXT("invalid appearance dimensions"));
    for(int32 A=0;A<3;++A)if(FMath::Abs(int64(O[A]))>1000000)return Fail(TEXT("invalid appearance origin"));
    uint8 Hash[SHA256_DIGEST_LENGTH];
    TArray<uint8> Checked;Checked.Reserve(Bytes.Num()-32);Checked.Append(P,96);Checked.Append(P+HeaderBytes,Bytes.Num()-HeaderBytes);
    if(!SHA256(Checked.GetData(),Checked.Num(),Hash)||FMemory::Memcmp(Hash,P+96,32))return Fail(TEXT("appearance checksum mismatch"));
    int64 Previous=-1;
    for(uint32 I=0;I<N;++I){
        const uint8* R=P+HeaderBytes+int64(I)*RecordBytes;const FIntVector C=XYZ(R);
        const int64 Key=(int64(C.X)*S.Y+C.Y)*S.Z+C.Z;
        if(C.X>=S.X||C.Y>=S.Y||C.Z>=S.Z||Key<=Previous||(Version==1?(R[6]<16||R[6]>25):(R[6]==0||uint32(R[6])>=uint32(vxc::kMaterialCount))))return Fail(TEXT("invalid or duplicate appearance coordinate/material"));
        Previous=Key;
    }
    auto Result=MakeShared<FVoxelAssetAppearance,ESPMode::ThreadSafe>();
    Result->PacketVersion=Version;Result->MaskEnabled=Version==1||(Flags&1u)!=0;
    Result->Size=S;Result->Origin=O;Result->Mm=Version==1?double(Pitch):double(Pitch)/1000.;Result->Count=int32(N);Result->Needle=U32(P+40)!=0;Result->Data=MoveTemp(Bytes);
    return Result;
}

TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> FVoxelAssetAppearance::Load(const FString& SourceMD5)
{
    if(!ValidHash(SourceMD5))return nullptr;
    FString Directory=FPaths::ProjectContentDir()/TEXT("Data/AssetAppearance");
    FString AssetDirectory;
    if(FParse::Value(FCommandLine::Get(),TEXT("VoxelAssetDir="),AssetDirectory)&&!AssetDirectory.IsEmpty())
        Directory=AssetDirectory/TEXT("appearance");
    FParse::Value(FCommandLine::Get(),TEXT("VoxelAssetAppearanceDir="),Directory);
    const FString Path=FPaths::ConvertRelativePathToFull(Directory/(SourceMD5.ToLower()+TEXT(".vac")));
    static FCriticalSection Mutex;
    static TMap<FString,TWeakPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe>> Cache;
    FScopeLock Lock(&Mutex);
    if(auto Existing=Cache.Find(Path))if(auto Value=Existing->Pin())return Value;
    const int64 Size=IFileManager::Get().FileSize(*Path);
    if(Size<0)return nullptr; // Legacy/unreviewed assets keep their palette.
    if(Size<HeaderBytes||Size>MaxBytes){UE_LOG(LogVoxelEarth,Warning,TEXT("AssetAppearance refused oversized/invalid file %s"),*Path);return nullptr;}
    TArray<uint8> Bytes;if(!FFileHelper::LoadFileToArray(Bytes,*Path))return nullptr;
    FString Error;auto Value=Parse(MoveTemp(Bytes),SourceMD5,Error);
    if(!Value){UE_LOG(LogVoxelEarth,Warning,TEXT("AssetAppearance refused %s: %s"),*Path,*Error);return nullptr;}
    Cache.Add(Path,Value);
    UE_LOG(LogVoxelEarth,Log,TEXT("AssetAppearance loaded source=%s voxels=%d"),*SourceMD5,Value->PointCount());
    return Value;
}

TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> FVoxelAssetAppearance::ForCanonicalYaw(TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> Source,uint8 Yaw)
{
    if(!Source||Yaw>3||Source->Original)return nullptr;
    if(!Yaw)return Source;
    auto View=MakeShared<FVoxelAssetAppearance,ESPMode::ThreadSafe>();
    View->Size=Source->Size;View->Origin=Source->Origin;View->Mm=Source->Mm;
    View->Count=Source->Count;View->Needle=Source->Needle;View->MaskEnabled=Source->MaskEnabled;View->PacketVersion=Source->PacketVersion;
    View->CanonicalYaw=Yaw;View->Original=MoveTemp(Source);return View;
}

const uint8* FVoxelAssetAppearance::Find(const FIntVector& Cell) const
{
    if(Original)return Original->Find(Cell);
    if(Cell.GetMin()<0||Cell.X>=Size.X||Cell.Y>=Size.Y||Cell.Z>=Size.Z)return nullptr;
    const int64 Key=(int64(Cell.X)*Size.Y+Cell.Y)*Size.Z+Cell.Z;
    int32 Lo=0,Hi=Count;
    while(Lo<Hi){const int32 Mid=Lo+(Hi-Lo)/2;const uint8* R=Data.GetData()+HeaderBytes+int64(Mid)*RecordBytes;const auto C=XYZ(R);
        const int64 K=(int64(C.X)*Size.Y+C.Y)*Size.Z+C.Z;
        if(K<Key)Lo=Mid+1;else Hi=Mid;
    }
    if(Lo==Count)return nullptr;
    const uint8* R=Data.GetData()+HeaderBytes+int64(Lo)*RecordBytes;
    return XYZ(R)==Cell?R:nullptr;
}

bool FVoxelAssetAppearance::Sample(const FIntVector& AssetCell,double GridMm,uint8 Material,FColor& Color,FIntVector& SourceCell) const
{
    if(!FMath::IsFinite(GridMm)||Mm<=0||GridMm<Mm||GridMm/Mm>8)return false;
    const double Scale=GridMm/Mm;const int32 Ratio=FMath::RoundToInt(Scale);
    if(Ratio<1||FMath::Abs(Scale-Ratio)>1.e-9)return false;
    if(Original){
        // Transform each represented fine cell, then choose the first source
        // record in source order. A rotated coarse voxel must not choose a
        // different child just because world-axis iteration changed.
        const int64 First[3]={int64(AssetCell.X)*Ratio,int64(AssetCell.Y)*Ratio,int64(AssetCell.Z)*Ratio};
        const uint8* Best=nullptr;int64 BestKey=MAX_int64;
        for(int32 X=0;X<Ratio;++X)for(int32 Y=0;Y<Ratio;++Y)for(int32 Z=0;Z<Ratio;++Z){
            const int64 WX=First[0]+X,WY=First[1]+Y,WZ=First[2]+Z;
            const int64 SX=(CanonicalYaw==1?WY:CanonicalYaw==2?-WX:-WY)-Origin.X;
            const int64 SY=(CanonicalYaw==1?-WX:CanonicalYaw==2?-WY:WX)-Origin.Y;
            const int64 SZ=WZ-Origin.Z;
            if(SX<0||SY<0||SZ<0||SX>=Size.X||SY>=Size.Y||SZ>=Size.Z)continue;
            const FIntVector S{int32(SX),int32(SY),int32(SZ)};const uint8* R=Find(S);
            if(R&&R[6]==Material){const int64 Key=(int64(S.X)*Size.Y+S.Y)*Size.Z+S.Z;
                if(Key<BestKey){Best=R;BestKey=Key;SourceCell=S;}}
        }
        if(!Best)return false;
        Color=FColor(Best[7],Best[8],Best[9]);return true;
    }
    FIntVector First;
    for(int32 A=0;A<3;++A){const int64 Value=int64(AssetCell[A])*Ratio-Origin[A];if(Value<=-Ratio||Value>=Size[A])return false;First[A]=int32(Value);}
    // At coarse LOD, choose a matching represented child deterministically.
    // Added/replaced material never inherits an unrelated source voxel color.
    for(int32 X=0;X<Ratio;++X)for(int32 Y=0;Y<Ratio;++Y)for(int32 Z=0;Z<Ratio;++Z){
        const auto C=First+FIntVector(X,Y,Z);const uint8* R=Find(C);
        if(R&&R[6]==Material){Color=FColor(R[7],R[8],R[9]);SourceCell=C;return true;}
    }
    return false;
}

FLinearColor FVoxelAssetAppearance::FaceColor(FColor Base,const FIntVector& C,int32 Axis,bool Positive)
{
    const uint32 Key=uint32(C.X)*73856093u^uint32(C.Y)*19349663u^uint32(C.Z)*83492791u;
    const float Gain=1.f+.085f*Unit(Key)+.045f*Unit(Key^((uint32(Axis)*2u+uint32(Positive)+1u)*2654435761u));
    const float Warmth=Unit(Key^1597334677u);
    auto Decode=[](float V){V=FMath::Clamp(V,0.f,1.f);return V<=.04045f?V/12.92f:FMath::Pow((V+.055f)/1.055f,2.4f);};
    return FLinearColor(Decode(float(Base.R)/255.f*Gain*(1.f+.025f*Warmth)),Decode(float(Base.G)/255.f*Gain),Decode(float(Base.B)/255.f*Gain*(1.f-.025f*Warmth)),1.f);
}

FVector2f FVoxelAssetAppearance::FaceUV(const FVector3f& P,int32 Axis) const
{
    FVector3f SourcePosition=P;const float CellUU=float(Mm)*.1f;
    if(CanonicalYaw==1){SourcePosition=FVector3f(P.Y,CellUU-P.X,P.Z);if(Axis<2)Axis=1-Axis;}
    else if(CanonicalYaw==2)SourcePosition=FVector3f(CellUU-P.X,CellUU-P.Y,P.Z);
    else if(CanonicalYaw==3){SourcePosition=FVector3f(CellUU-P.Y,P.X,P.Z);if(Axis<2)Axis=1-Axis;}
    const FVector3f Local=SourcePosition*.01f-FVector3f(Origin)*float(Mm)*.001f;
    return Axis==2?FVector2f(Local.X,Local.Y):Axis==0?FVector2f(Local.Y,Local.Z):FVector2f(Local.X,Local.Z);
}

FLinearColor FVoxelAssetAppearance::ColorForFace(FColor Base,const FIntVector& SourceCell,int32 Axis,bool Positive) const
{
    if(Axis<2){
        if(CanonicalYaw==2||(CanonicalYaw==1&&Axis==0)||(CanonicalYaw==3&&Axis==1))Positive=!Positive;
        if(CanonicalYaw&1u)Axis=1-Axis;
    }
    return FaceColor(Base,SourceCell,Axis,Positive);
}

TSharedPtr<const FVoxelSparseAppearance,ESPMode::ThreadSafe> FVoxelAssetAppearance::BuildSparseResource(
    FString& Error,uint64 MaxOutputBytes,uint64 MaxWorkingBytes) const
{
    Error.Reset();
    auto Fail=[&](const TCHAR* Why)->TSharedPtr<const FVoxelSparseAppearance,ESPMode::ThreadSafe>{Error=Why;return nullptr;};
    if(Original||CanonicalYaw)return Fail(TEXT("sparse resource requires the original source frame"));
    FScopeLock Lock(&SparseMutex);
    if(Sparse)return Sparse->RequiredOutputBytes<=MaxOutputBytes&&Sparse->RequiredWorkingBytes<=MaxWorkingBytes?Sparse:Fail(TEXT("cached sparse resource exceeds budget"));
    if(Count<=0||Data.Num()<HeaderBytes)return Fail(TEXT("missing verified source records"));
    // Reserve explicitly and budget conservative allocator slack before allocating.
    // 50% + 256 bytes covers default TArray reserve/growth quantization; actual
    // allocated sizes are verified too. Source VAC1 remains independently owned.
    auto BudgetBytes=[](uint64 Raw){return Raw+Raw/2+256;};
    const uint64 ScratchBytes=BudgetBytes(uint64(Count)*sizeof(uint32));
    if(ScratchBytes>MaxWorkingBytes)return Fail(TEXT("sparse sort exceeds working budget"));
    TArray<uint32> Order;Order.Reserve(Count);Order.SetNumUninitialized(Count);
    if(Order.GetAllocatedSize()>ScratchBytes)return Fail(TEXT("sort allocator exceeded reserved budget"));
    for(int32 I=0;I<Count;++I)Order[I]=uint32(I);
    auto Record=[&](uint32 I){return Data.GetData()+HeaderBytes+uint64(I)*RecordBytes;};
    auto BrickKey=[&](uint32 I){const FIntVector C=XYZ(Record(I));return (uint64(uint32(C.X)>>3)<<26)|(uint64(uint32(C.Y)>>3)<<13)|uint64(uint32(C.Z)>>3);};
    auto Local=[&](uint32 I){const FIntVector C=XYZ(Record(I));return uint32((C.X&7)+8*(C.Y&7)+64*(C.Z&7));};
    Order.Sort([&](uint32 A,uint32 B){const uint64 KA=BrickKey(A),KB=BrickKey(B);return KA<KB||(KA==KB&&Local(A)<Local(B));});
    uint32 Bricks=0;uint64 Previous=MAX_uint64;
    for(uint32 I:Order){const uint64 K=BrickKey(I);if(K!=Previous){++Bricks;Previous=K;}}
    const uint64 TotalWords=FVoxelSparseAppearance::HeaderWords+uint64(Bricks)*20+uint64(Count),Bytes=BudgetBytes(TotalWords*4);
    if(TotalWords>MAX_int32||Bytes>MaxOutputBytes||Bytes>MaxWorkingBytes||ScratchBytes>MaxWorkingBytes-Bytes)
        return Fail(TEXT("sparse output or combined working budget exceeded"));
    auto Result=MakeShared<FVoxelSparseAppearance,ESPMode::ThreadSafe>();auto& W=Result->Words;
    W.Reserve(int32(TotalWords));W.SetNumZeroed(int32(TotalWords));
    if(W.GetAllocatedSize()>Bytes)return Fail(TEXT("output allocator exceeded reserved budget"));
    Result->RequiredOutputBytes=Bytes;Result->RequiredWorkingBytes=ScratchBytes+Bytes;
    W[0]=PacketVersion;W[1]=uint32(TotalWords);W[2]=uint32(Count);W[3]=Bricks;
    W[4]=FVoxelSparseAppearance::HeaderWords;W[5]=W[4]+Bricks*4;W[6]=W[5]+Bricks*16;
    for(int A=0;A<3;++A){W[7+A]=uint32(Size[A]);W[10+A]=uint32(Origin[A]);}
    W[13]=PacketVersion==1?uint32(Mm):uint32(FMath::RoundToInt(Mm*1000.));W[14]=Needle?1u:0u;W[15]=PacketVersion==1?0u:(MaskEnabled?1u:0u);for(int I=0;I<4;++I)W[16+I]=U32(Data.GetData()+48+I*4);
    Previous=MAX_uint64;uint32 Brick=0,OutRecord=0;
    for(uint32 I:Order){const uint64 K=BrickKey(I);const uint8* R=Record(I);const FIntVector C=XYZ(R);
        if(K!=Previous){if(Previous!=MAX_uint64)++Brick;Previous=K;const uint32 D=W[4]+Brick*4;
            W[D]=uint32(C.X)>>3;W[D+1]=uint32(C.Y)>>3;W[D+2]=uint32(C.Z)>>3;W[D+3]=OutRecord;}
        const uint32 Bit=Local(I);W[W[5]+Brick*16+(Bit>>5)]|=1u<<(Bit&31u);
        W[W[6]+OutRecord]=uint32(R[6])|(uint32(R[7])<<8)|(uint32(R[8])<<16)|(uint32(R[9])<<24);++OutRecord;
    }
    Sparse=Result;return Sparse;
}
