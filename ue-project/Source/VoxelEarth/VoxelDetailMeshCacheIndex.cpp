#include "VoxelDetailMeshCacheIndex.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include <openssl/sha.h>

namespace {
FString CacheSHA(const uint8* P,int32 N){uint8 H[32];SHA256(P,size_t(N),H);return BytesToHex(H,32).ToLower();}
FString CacheTextSHA(const FString& S){FTCHARToUTF8 U(*S);return CacheSHA(reinterpret_cast<const uint8*>(U.Get()),U.Length());}
bool CacheHex(const FString& S,int32 N){if(S.Len()!=N)return false;for(TCHAR C:S)if(!((C>='0'&&C<='9')||(C>='a'&&C<='f')))return false;return true;}
bool CachePart(const FString& S){if(S.IsEmpty())return false;for(TCHAR C:S)if(!((C>='a'&&C<='z')||(C>='A'&&C<='Z')||(C>='0'&&C<='9')||C=='-'||C=='_'))return false;return true;}
bool CacheJson(const FString& S,TSharedPtr<FJsonObject>& O){return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(S),O)&&O.IsValid();}
bool CacheString(const TSharedPtr<FJsonObject>& O,const TCHAR* K,const FString& Expected){FString V;return O->TryGetStringField(K,V)&&V==Expected;}
bool CacheHash(const TSharedPtr<FJsonObject>& O,const TCHAR* K,FString& V){return O->TryGetStringField(K,V)&&CacheHex(V,64);}
bool CachePackageFile(const FString& File,const FString& Package){
    if(File.IsEmpty())return false;
    FString Expected=FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Package,TEXT(".uasset")));
    FString Actual=FPaths::ConvertRelativePathToFull(File);FPaths::NormalizeFilename(Expected);FPaths::NormalizeFilename(Actual);
    return Actual.Equals(Expected,ESearchCase::CaseSensitive);
}
}
TSharedPtr<const FVoxelDetailMeshCacheIndex,ESPMode::ThreadSafe> FVoxelDetailMeshCacheIndex::ParseEditorSource(
    const FString& Text,const TArray<uint8>& PublicationBytes,
    TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> Catalog,
    const FVoxelDetailCacheIdentity& E,FString& Error){
    Error.Reset();auto Fail=[&](const TCHAR* Why)->TSharedPtr<const FVoxelDetailMeshCacheIndex,ESPMode::ThreadSafe>{Error=Why;return nullptr;};
#if !WITH_EDITOR
    return Fail(TEXT("uncooked detail index is editor-only"));
#else
    if(!Catalog||Text.Len()>16*1024*1024||PublicationBytes.IsEmpty()||PublicationBytes.Num()>16*1024*1024||
       E.EngineVersion.IsEmpty()||E.HostPlatform.IsEmpty()||E.Settings.IsEmpty()||E.BuilderIdentity.IsEmpty()||!CacheHex(E.MaterialSourceSHA256,64))return Fail(TEXT("missing expected cache identity"));
    TSharedPtr<FJsonObject> Index,Publication;
    FUTF8ToTCHAR P(reinterpret_cast<const ANSICHAR*>(PublicationBytes.GetData()),PublicationBytes.Num());
    if(!CacheJson(Text,Index)||!CacheJson(FString(P.Length(),P.Get()),Publication))return Fail(TEXT("malformed cache or publication JSON"));
    double Schema=0,FingerprintSchema=0;bool Preview=false;
    if(!Index->TryGetNumberField(TEXT("schema"),Schema)||Schema!=2||!Index->TryGetNumberField(TEXT("render_attribute_fingerprint_schema"),FingerprintSchema)||FingerprintSchema!=1||!Index->TryGetBoolField(TEXT("preview_only"),Preview)||
       !CacheString(Index,TEXT("stage"),TEXT("uncooked-editor-source; platform cook and runtime acceptance pending")))return Fail(TEXT("unsupported cache stage or schema"));
    if(Preview!=E.bActivePublicationIsPreview||(Preview&&!E.bAllowPreview))return Fail(TEXT("preview cache isolation mismatch"));
    if(!CacheString(Index,TEXT("engine_version"),E.EngineVersion)||!CacheString(Index,TEXT("host_platform"),E.HostPlatform)||
       !CacheString(Index,TEXT("settings"),E.Settings)||!CacheString(Index,TEXT("builder_sha256"),CacheTextSHA(E.BuilderIdentity))||
       !CacheString(Index,TEXT("material_source_sha256"),E.MaterialSourceSHA256)||
       !CacheString(Index,TEXT("publication_sha256"),CacheSHA(PublicationBytes.GetData(),PublicationBytes.Num())))return Fail(TEXT("cache identity mismatch"));
    const TArray<TSharedPtr<FJsonValue>> *Published=nullptr,*Rows=nullptr;
    if(!Publication->TryGetArrayField(TEXT("models"),Published)||!Index->TryGetArrayField(TEXT("models"),Rows)||Published->Num()>65535||Rows->Num()>65535)return Fail(TEXT("invalid model arrays"));
    struct FSource {FString Species,Geometry,Appearance;uint32 Resource=0;};TMap<FString,FSource> Sources;
    for(const auto& V:*Published){
        if(!V.IsValid()||V->Type!=EJson::Object)return Fail(TEXT("invalid publication row"));const auto O=V->AsObject();FString ID,MD5;FSource S;
        if(!O->TryGetStringField(TEXT("id"),ID)||!CachePart(ID)||Sources.Contains(ID)||!O->TryGetStringField(TEXT("species"),S.Species)||!CachePart(S.Species)||
           !CacheHash(O,TEXT("geometry_sha256"),S.Geometry)||!CacheHash(O,TEXT("sha256"),S.Appearance)||
           !O->TryGetStringField(TEXT("geometry_md5"),MD5)||!CacheHex(MD5,32))return Fail(TEXT("invalid publication source identity"));
        S.Resource=Catalog->FindResource(MD5);if(!S.Resource||S.Resource>=uint32(Catalog->Sources().Num()))return Fail(TEXT("publication does not match catalog"));
        const auto& Verified=Catalog->Sources()[S.Resource];
        if(S.Geometry!=Verified.GeometrySHA256||S.Appearance!=Verified.AppearanceSHA256)return Fail(TEXT("publication hashes do not match retained catalog"));
        Sources.Add(ID,MoveTemp(S));
    }
    auto Result=MakeShared<FVoxelDetailMeshCacheIndex,ESPMode::ThreadSafe>();Result->Snapshot=MoveTemp(Catalog);TSet<FString> Seen;
    const FString Prefix=Preview?TEXT("/Game/Voxel/Generated/DetailPreview/"):TEXT("/Game/Voxel/Generated/Detail/");
    for(const auto& V:*Rows){
        if(!V.IsValid()||V->Type!=EJson::Object)return Fail(TEXT("invalid cache row"));const auto O=V->AsObject();FString ID,Species,PackageFile,MaterialFile;FEntry Entry;double Pitch=0;
        if(!O->TryGetStringField(TEXT("id"),ID)||Seen.Contains(ID)||!O->TryGetStringField(TEXT("species"),Species))return Fail(TEXT("duplicate or invalid cache identity"));
        const FSource* S=Sources.Find(ID);if(!S||S->Species!=Species)return Fail(TEXT("cache source not published"));Seen.Add(ID);Entry.Resource=S->Resource;
        if(!CacheHash(O,TEXT("geometry_sha256"),Entry.GeometrySHA256)||Entry.GeometrySHA256!=S->Geometry||
           !CacheHash(O,TEXT("appearance_sha256"),Entry.AppearanceSHA256)||Entry.AppearanceSHA256!=S->Appearance||
           !CacheHash(O,TEXT("derived_key"),Entry.DerivedKey)||!CacheHash(O,TEXT("package_sha256"),Entry.PackageSHA256)||!CacheHash(O,TEXT("material_sha256"),Entry.MaterialSHA256))return Fail(TEXT("cache source hashes mismatch"));
        if(Entry.DerivedKey!=CacheTextSHA(S->Geometry+TEXT("\n")+S->Appearance+TEXT("\n")+E.BuilderIdentity+E.Settings+E.EngineVersion+E.MaterialSourceSHA256+E.HostPlatform))return Fail(TEXT("derived key mismatch"));
        const auto& Source=Result->Snapshot->Sources()[Entry.Resource];
        if(!Source.Appearance||!O->TryGetNumberField(TEXT("voxel_pitch_um"),Pitch)||!FMath::IsFinite(Pitch)||Pitch<=0||Pitch>1000000||Pitch!=FMath::FloorToDouble(Pitch)||Pitch!=Source.Appearance->PitchMm()*1000.0)return Fail(TEXT("source pitch mismatch"));Entry.PitchUm=uint32(Pitch);
        const TSharedPtr<FJsonObject>* Facts=nullptr;const TArray<TSharedPtr<FJsonValue>>* Lods=nullptr;double SourceModels=0;
        if(!O->TryGetObjectField(TEXT("mesh_facts"),Facts)||!Facts->IsValid()||!(*Facts)->TryGetArrayField(TEXT("lods"),Lods)||Lods->Num()<1||Lods->Num()>3||
           !(*Facts)->TryGetNumberField(TEXT("source_models"),SourceModels)||SourceModels!=Lods->Num()||!(*Facts)->TryGetStringField(TEXT("bounds"),Entry.Bounds)||Entry.Bounds.IsEmpty()||Entry.Bounds.Len()>1024)return Fail(TEXT("invalid mesh facts"));
        if(!(*Facts)->TryGetNumberField(TEXT("fingerprint_schema"),FingerprintSchema)||FingerprintSchema!=1||!CacheHash(*Facts,TEXT("attribute_sha256"),Entry.AttributeSHA256))return Fail(TEXT("missing render attribute fingerprint"));
        double LastScreen=2;
        for(const auto& L:*Lods){
            if(!L.IsValid()||L->Type!=EJson::Object)return Fail(TEXT("invalid LOD facts"));auto LO=L->AsObject();double Vertices=0,Triangles=0,UV=0,Screen=0;
            if(!LO->TryGetNumberField(TEXT("vertices"),Vertices)||!LO->TryGetNumberField(TEXT("triangles"),Triangles)||!LO->TryGetNumberField(TEXT("uv_channels"),UV)||!LO->TryGetNumberField(TEXT("screen_size"),Screen)||
               !FMath::IsFinite(Vertices)||Vertices<3||Vertices>MAX_int32||Vertices!=FMath::FloorToDouble(Vertices)||
               !FMath::IsFinite(Triangles)||Triangles<1||Triangles>MAX_int32/3||Triangles!=FMath::FloorToDouble(Triangles)||
               UV!=2||!FMath::IsFinite(Screen)||Screen<=0||Screen>1||Screen>=LastScreen)return Fail(TEXT("invalid LOD geometry counts"));
            Entry.Lods.Add({uint32(Vertices),uint32(Triangles),uint32(UV),Screen});LastScreen=Screen;
        }
        if(!O->TryGetStringField(TEXT("object_path"),Entry.ObjectPath)||!Entry.ObjectPath.StartsWith(Prefix))return Fail(TEXT("unsafe cache object path"));
        const FString Tail=Entry.ObjectPath.RightChop(Prefix.Len());FString Run,Asset;
        if(!Tail.Split(TEXT("/"),&Run,&Asset)||!CachePart(Run)||Asset!=TEXT("D_")+Entry.DerivedKey+TEXT(".D_")+Entry.DerivedKey)return Fail(TEXT("invalid cache package shape"));
        const FString Package=Prefix+Run+TEXT("/D_")+Entry.DerivedKey,MaterialPackage=Prefix+Run+TEXT("/M_")+Entry.DerivedKey;
        if(!O->TryGetStringField(TEXT("material_object_path"),Entry.MaterialObjectPath)||Entry.MaterialObjectPath!=MaterialPackage+TEXT(".M_")+Entry.DerivedKey)return Fail(TEXT("material binding path mismatch"));
        if(!O->TryGetStringField(TEXT("package_file"),PackageFile)||!O->TryGetStringField(TEXT("material_file"),MaterialFile)||!CachePackageFile(PackageFile,Package)||!CachePackageFile(MaterialFile,MaterialPackage))return Fail(TEXT("cache file path differs from object path"));
        Entry.PackageFile=PackageFile;Entry.MaterialFile=MaterialFile;
        if(const auto* Previous=Result->Entries.Find(Entry.Resource)){
            if(Previous->DerivedKey!=Entry.DerivedKey||Previous->ObjectPath!=Entry.ObjectPath||Previous->MaterialObjectPath!=Entry.MaterialObjectPath||Previous->PackageSHA256!=Entry.PackageSHA256||Previous->MaterialSHA256!=Entry.MaterialSHA256||Previous->AttributeSHA256!=Entry.AttributeSHA256||Previous->PitchUm!=Entry.PitchUm||Previous->Bounds!=Entry.Bounds||Previous->Lods.Num()!=Entry.Lods.Num())return Fail(TEXT("conflicting cache resource alias"));
            for(int32 I=0;I<Entry.Lods.Num();++I){const auto& A=Previous->Lods[I];const auto& B=Entry.Lods[I];
                if(A.Vertices!=B.Vertices||A.Triangles!=B.Triangles||A.UVChannels!=B.UVChannels||A.ScreenSize!=B.ScreenSize)return Fail(TEXT("conflicting cache LOD alias"));}
        }else Result->Entries.Add(Entry.Resource,MoveTemp(Entry));
    }
    return Result;
#endif
}
