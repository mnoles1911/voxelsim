#include "VoxelPublishedAppearanceCatalog.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include <openssl/sha.h>
#include "voxelcore/assetgrid.h"
namespace {
uint32 Read32(const uint8* P){return uint32(P[0])|(uint32(P[1])<<8)|(uint32(P[2])<<16)|(uint32(P[3])<<24);}
uint32 Read16(const uint8* P){return uint32(P[0])|(uint32(P[1])<<8);}
uint64 AllocationBound(uint64 N){return N+N/2+256;}
bool Hex(const FString& S,int32 N){if(S.Len()!=N)return false;for(TCHAR C:S)if(!FChar::IsHexDigit(C))return false;return true;}
bool SafeSpecies(const FString& S){if(S.IsEmpty()||S.Len()>128)return false;for(TCHAR C:S)if(!((C>='a'&&C<='z')||(C>='0'&&C<='9')||C=='-'))return false;return true;}
bool ReadBounded(const FString& P,TArray<uint8>& B,int64 Limit){const int64 N=IFileManager::Get().FileSize(*P);return N>0&&N<=Limit&&FFileHelper::LoadFileToArray(B,*P)&&B.Num()==N;}
bool SHAEquals(const TArray<uint8>& B,const FString& Expected){uint8 H[32];return Hex(Expected,64)&&SHA256(B.GetData(),B.Num(),H)&&BytesToHex(H,32).Equals(Expected,ESearchCase::IgnoreCase);}
}
uint32 FVoxelPublishedAppearanceCatalog::FindResource(const FString& Hash) const {
    const uint32* ID=ByHash.Find(Hash.ToLower());return ID?*ID:0;
}
TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> FVoxelPublishedAppearanceCatalog::Load(
    const FString& Directory,FString& Error,uint64 MaxSourceBytes,uint64 MaxWorkingBytes) {
    Error.Reset();auto Fail=[&](const FString& Why)->TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe>{Error=Why;return nullptr;};
    const FString ManifestPath=Directory/TEXT("appearance/published.json");
    const int64 ManifestSize=IFileManager::Get().FileSize(*ManifestPath);
    FString Text;TSharedPtr<FJsonObject> Json;
    if(ManifestSize<=0||ManifestSize>16*1024*1024||!FFileHelper::LoadFileToString(Text,*ManifestPath)||
       !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json)||!Json)return Fail(TEXT("invalid publication inventory"));
    const TArray<TSharedPtr<FJsonValue>>* Models=nullptr;
    if(!Json->TryGetArrayField(TEXT("models"),Models)||Models->Num()>65535)return Fail(TEXT("invalid publication models"));
    struct FRow {FString ID,Species,MD5,GeometrySHA,AppearanceSHA;};TArray<FRow> Rows;TSet<FString> IDs;
    for(const auto& Value:*Models){
        if(!Value.IsValid()||Value->Type!=EJson::Object)return Fail(TEXT("invalid publication row"));
        auto O=Value->AsObject();FRow R;double Seed=0;FString File;
        if(!O->TryGetStringField(TEXT("id"),R.ID)||!O->TryGetStringField(TEXT("species"),R.Species)||!SafeSpecies(R.Species)||
           !O->TryGetNumberField(TEXT("seed"),Seed)||!FMath::IsFinite(Seed)||Seed<1||Seed>2147483647.0||Seed!=double(int32(Seed))||
           R.ID!=FString::Printf(TEXT("%s-%04d"),*R.Species,int32(Seed))||IDs.Contains(R.ID)||
           !O->TryGetStringField(TEXT("geometry_md5"),R.MD5)||!Hex(R.MD5,32)||
           !O->TryGetStringField(TEXT("geometry_sha256"),R.GeometrySHA)||!Hex(R.GeometrySHA,64)||
           !O->TryGetStringField(TEXT("sha256"),R.AppearanceSHA)||!Hex(R.AppearanceSHA,64)||
           !O->TryGetStringField(TEXT("file"),File)||File!=R.MD5.ToLower()+TEXT(".vac"))return Fail(TEXT("invalid or duplicate publication identity"));
        R.MD5=R.MD5.ToLower();IDs.Add(R.ID);Rows.Add(MoveTemp(R));
    }
    Rows.Sort([](const FRow& A,const FRow& B){return A.ID<B.ID;});
    auto Result=MakeShared<FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe>();Result->Entries.AddDefaulted();
    for(const FRow& R:Rows){
        TArray<uint8> Geometry,Packet;
        const int64 GN=IFileManager::Get().FileSize(*(Directory/TEXT("banks")/R.Species/(R.ID+TEXT(".vxa"))));
        const int64 PN=IFileManager::Get().FileSize(*(Directory/TEXT("appearance")/(R.MD5+TEXT(".vac"))));
        // Bound file arrays, packet checksum copy and conservative RLE parser
        // storage before reading. Column-index allocation is checked below.
        if(GN<48||GN>128ll*1024*1024||PN<128||PN>256ll*1024*1024)return Fail(TEXT("invalid published file sizes"));
        const uint64 BaseWorking=Result->Bytes+AllocationBound(uint64(GN))*4+AllocationBound(uint64(PN))*2;
        if(BaseWorking>MaxWorkingBytes)return Fail(TEXT("publication working budget exceeded"));
        if(!ReadBounded(Directory/TEXT("banks")/R.Species/(R.ID+TEXT(".vxa")),Geometry,128ll*1024*1024)||
           !FMD5::HashBytes(Geometry.GetData(),Geometry.Num()).Equals(R.MD5,ESearchCase::IgnoreCase)||!SHAEquals(Geometry,R.GeometrySHA))return Fail(TEXT("published geometry mismatch: ")+R.ID);
        if(!ReadBounded(Directory/TEXT("appearance")/(R.MD5+TEXT(".vac")),Packet,256ll*1024*1024)||!SHAEquals(Packet,R.AppearanceSHA))return Fail(TEXT("published appearance mismatch: ")+R.ID);
        if(Packet.Num()<128)return Fail(TEXT("invalid appearance header"));
        const uint32 SX=Read32(Geometry.GetData()+20),SY=Read32(Geometry.GetData()+24);
        if(SX>65535||SY>65535)return Fail(TEXT("invalid geometry dimensions"));
        const uint64 Working=BaseWorking+AllocationBound(uint64(SX)*SY*16);
        if(Working>MaxWorkingBytes)return Fail(TEXT("publication geometry working budget exceeded"));
        vxc::AssetGrid Grid;
        if(Grid.parse(Geometry.GetData(),size_t(Geometry.Num()))!=vxc::AssetParseError::kOk)return Fail(TEXT("invalid published VXA"));
        const uint8* P=Packet.GetData();
        if(Read32(P+8)!=uint32(Grid.sizeX())||Read32(P+12)!=uint32(Grid.sizeY())||Read32(P+16)!=uint32(Grid.sizeZ())||
           int32(Read32(P+20))!=Grid.originX()||int32(Read32(P+24))!=Grid.originY()||int32(Read32(P+28))!=Grid.originZ()||
           uint64(Read32(P+32))*(Read32(P+4)==1?1000u:1u)!=Grid.voxelSizeUm()||Read32(P+36)!=Grid.solidCount()||
           128ull+uint64(Read32(P+36))*10!=uint64(Packet.Num()))return Fail(TEXT("appearance geometry layout mismatch"));
        uint32 Record=0;bool Matches=true;
        for(int32 X=0;X<Grid.sizeX();++X)for(int32 Y=0;Y<Grid.sizeY();++Y)Grid.columnRuns(X,Y,[&](int32 Z,int32 Len,vxc::MaterialId M){
            if(M==vxc::MAT_AIR)return;
            for(int32 K=0;K<Len;++K){
                if(Record>=Read32(P+36)){Matches=false;return;}
                const uint8* Cell=P+128+uint64(Record++)*10;
                if(Read16(Cell)!=uint32(X)||Read16(Cell+2)!=uint32(Y)||Read16(Cell+4)!=uint32(Z+K)||Cell[6]!=uint8(M))Matches=false;
            }
        });
        if(!Matches)return Fail(TEXT("appearance geometry cell mismatch"));
        // Verify every row, including duplicate geometries; deduplicate only after
        // its files passed. No sidecar or cache can authorize a missing row.
        if(const uint32* Existing=Result->ByHash.Find(R.MD5)){
            const auto& Previous=Result->Entries[*Existing];
            if(!Previous.GeometrySHA256.Equals(R.GeometrySHA,ESearchCase::IgnoreCase)||
               !Previous.AppearanceSHA256.Equals(R.AppearanceSHA,ESearchCase::IgnoreCase))
                return Fail(TEXT("conflicting publication aliases for geometry: ")+R.MD5);
            continue;
        }
        const uint64 PacketBytes=uint64(Packet.Num());
        if(PacketBytes>MaxSourceBytes-Result->Bytes)return Fail(TEXT("publication source budget exceeded"));
        FString Why;auto Appearance=FVoxelAssetAppearance::Parse(MoveTemp(Packet),R.MD5,Why);
        if(!Appearance)return Fail(R.ID+TEXT(": ")+Why);
        auto Sparse=Appearance->BuildSparseResource(Why,MaxSourceBytes-Result->Bytes-PacketBytes,MaxWorkingBytes-Working);
        if(!Sparse)return Fail(FString::Printf(TEXT("%s: %s (catalog=%llu packet=%llu remainingOutput=%llu remainingWorking=%llu bytes)"),
            *R.ID,*Why,Result->Bytes,PacketBytes,MaxSourceBytes-Result->Bytes-PacketBytes,MaxWorkingBytes-Working));
        const uint64 Added=PacketBytes+Sparse->RequiredOutputBytes;
        if(Added>MaxSourceBytes-Result->Bytes)return Fail(TEXT("publication source budget exceeded"));
        const uint32 ID=uint32(Result->Entries.Num());Result->Entries.Add({R.MD5,Appearance,Sparse,R.GeometrySHA.ToLower(),R.AppearanceSHA.ToLower()});Result->ByHash.Add(R.MD5,ID);Result->Bytes+=Added;
    }
    return Result;
}
