#include "VoxelBakeDetailMeshesCommandlet.h"
#if WITH_EDITOR
#include "VoxelDetailMeshBake.h"
#include "VoxelMeshAttributeFingerprint.h"
#include "VoxelPublishedAppearanceCatalog.h"
#include "voxelcore/assetgrid.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "StaticMeshCompiler.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProperties.h"
#include <openssl/sha.h>

namespace {
FString Digest(const TArray<uint8>& Bytes){uint8 Hash[32];SHA256(Bytes.GetData(),size_t(Bytes.Num()),Hash);return BytesToHex(Hash,32).ToLower();}
FString TextDigest(const FString& Text){FTCHARToUTF8 U(*Text);TArray<uint8> B;B.Append(reinterpret_cast<const uint8*>(U.Get()),U.Length());return Digest(B);}
bool FileDigest(const FString& Path,FString& Hash){TArray<uint8> B;if(!FFileHelper::LoadFileToArray(B,*Path))return false;Hash=Digest(B);return true;}
bool ReadJson(const FString& Path,TSharedPtr<FJsonObject>& Out){FString T;return FFileHelper::LoadFileToString(T,*Path)&&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(T),Out)&&Out.IsValid();}
bool WriteJson(const FString& Path,const TSharedRef<FJsonObject>& O){FString T;if(!FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&T)))return false;return FFileHelper::SaveStringToFile(T,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);}
bool SafePart(const FString& S){if(S.IsEmpty())return false;for(TCHAR C:S)if(!FChar::IsAlnum(C)&&C!='-'&&C!='_')return false;return true;}
TSharedPtr<FJsonObject> MeshFacts(UStaticMesh* M,FString& Error,bool Attributes=true){
    if(!M||!M->GetRenderData()){Error=TEXT("Missing mesh render data");return nullptr;}
    FVoxelMeshAttributeFingerprint Fingerprint;
    if(Attributes&&!VoxelFingerprintMeshAttributes(*M->GetRenderData(),Fingerprint,Error))return nullptr;
    auto O=MakeShared<FJsonObject>();TArray<TSharedPtr<FJsonValue>> Lods;
    for(int32 L=0;L<M->GetRenderData()->LODResources.Num();++L){const auto& R=M->GetRenderData()->LODResources[L];auto V=MakeShared<FJsonObject>();
        V->SetNumberField(TEXT("vertices"),R.GetNumVertices());V->SetNumberField(TEXT("triangles"),R.GetNumTriangles());
        V->SetNumberField(TEXT("uv_channels"),R.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords());
        V->SetNumberField(TEXT("screen_size"),M->GetRenderData()->ScreenSize[L].Default);
        if(Attributes){const auto& F=Fingerprint.Lods[L];
            V->SetStringField(TEXT("attribute_sha256"),F.Sha256);V->SetNumberField(TEXT("sections"),F.Sections);
            V->SetNumberField(TEXT("main_indices"),F.MainIndices);V->SetNumberField(TEXT("depth_indices"),F.DepthIndices);
            V->SetNumberField(TEXT("reversed_indices"),F.ReversedIndices);V->SetNumberField(TEXT("reversed_depth_indices"),F.ReversedDepthIndices);
            V->SetNumberField(TEXT("wireframe_indices"),F.WireframeIndices);}
        Lods.Add(MakeShared<FJsonValueObject>(V));}
    O->SetArrayField(TEXT("lods"),Lods);O->SetNumberField(TEXT("source_models"),M->GetNumSourceModels());
    O->SetStringField(TEXT("bounds"),M->GetBounds().ToString());
    if(Attributes){O->SetNumberField(TEXT("fingerprint_schema"),FVoxelMeshAttributeFingerprint::SchemaVersion);O->SetStringField(TEXT("attribute_sha256"),Fingerprint.Sha256);}
    Error.Reset();return O;
}
bool SaveAsset(UPackage* P,UObject* A,const FString& PackageName,FString& Filename){
    Filename=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename),true);
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
    return UPackage::SavePackage(P,A,*Filename,Args);
}
}
#endif

UVoxelBakeDetailMeshesCommandlet::UVoxelBakeDetailMeshesCommandlet(){IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}
int32 UVoxelBakeDetailMeshesCommandlet::Main(const FString& Params){
#if !WITH_EDITOR
    return 1;
#else
    auto Fail=[](const FString& Why){UE_LOG(LogTemp,Error,TEXT("DetailBake: %s"),*Why);return 1;};
    FString Verify;
    if(FParse::Value(*Params,TEXT("VerifyManifest="),Verify)){
        TSharedPtr<FJsonObject> Manifest;if(!ReadJson(Verify,Manifest))return Fail(TEXT("invalid verification manifest"));
        const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;
        const int32 Schema=Manifest->GetIntegerField(TEXT("schema"));
        if((Schema!=1&&Schema!=2)||!Manifest->TryGetArrayField(TEXT("models"),Rows))return Fail(TEXT("unsupported manifest"));
        const bool Strong=Schema==2;
        if(Strong&&Manifest->GetIntegerField(TEXT("render_attribute_fingerprint_schema"))!=int32(FVoxelMeshAttributeFingerprint::SchemaVersion))return Fail(TEXT("unsupported attribute fingerprint contract"));
        if(Strong){FString BaseHash;
            if(!FileDigest(FPackageName::LongPackageNameToFilename(TEXT("/Game/Voxel/M_VoxelDetailAsset"),TEXT(".uasset")),BaseHash)||BaseHash!=Manifest->GetStringField(TEXT("material_source_sha256")))return Fail(TEXT("base material package changed"));}
        else UE_LOG(LogTemp,Warning,TEXT("DetailBake schema1 legacy verification: counts/bounds only; does NOT prove attribute persistence"));
        if(Manifest->GetStringField(TEXT("engine_version"))!=FEngineVersion::Current().ToString())return Fail(TEXT("engine mismatch"));
        for(const auto& V:*Rows){auto O=V->AsObject();FString Hash;if(!O)return Fail(TEXT("invalid model record"));
            if(!FileDigest(O->GetStringField(TEXT("package_file")),Hash)||Hash!=O->GetStringField(TEXT("package_sha256")))return Fail(TEXT("mesh package digest mismatch"));
            if(!FileDigest(O->GetStringField(TEXT("material_file")),Hash)||Hash!=O->GetStringField(TEXT("material_sha256")))return Fail(TEXT("material package digest mismatch"));
            auto* Mesh=LoadObject<UStaticMesh>(nullptr,*O->GetStringField(TEXT("object_path")));
            FStaticMeshCompilingManager::Get().FinishAllCompilation();FString FactsError;auto Actual=MeshFacts(Mesh,FactsError,Strong);
            if(!Actual)return Fail(TEXT("fresh-process mesh facts unavailable: ")+FactsError);
            const TSharedPtr<FJsonObject>* Expected=nullptr;
            if(!O->TryGetObjectField(TEXT("mesh_facts"),Expected)||!Expected||!Expected->IsValid())return Fail(TEXT("missing recorded mesh facts"));
            if(!FJsonValue::CompareEqual(FJsonValueObject(Actual),FJsonValueObject(*Expected)))return Fail(TEXT("reloaded mesh facts differ"));
            if(!Mesh->GetMaterial(0)||Mesh->GetMaterial(0)->HasAnyFlags(RF_Transient))return Fail(TEXT("missing or transient material"));
            if(Strong&&(!Mesh->bAllowCPUAccess||Mesh->GetMaterial(0)->GetPathName()!=O->GetStringField(TEXT("material_object_path"))))return Fail(TEXT("CPU retention or material binding changed"));
        }
        UE_LOG(LogTemp,Display,TEXT("DetailBake fresh-process verification PASS: %d models; schema%d %s (uncooked editor packages only, not source/pixel equivalence)"),Rows->Num(),Schema,Strong?TEXT("attribute persistence"):TEXT("legacy counts only"));return 0;
    }
    FString Directory,Library,Run;
    const bool Preview=FParse::Param(*Params,TEXT("PreviewOnly"));
    if(!FParse::Value(*Params,TEXT("Publication="),Directory)||!FParse::Value(*Params,TEXT("RunId="),Run)||!SafePart(Run))return Fail(TEXT("requires Publication and safe fresh RunId"));
    if(!Preview&&!FParse::Value(*Params,TEXT("ApprovedLibrary="),Library))return Fail(TEXT("authoritative bake requires ApprovedLibrary for endorsement verification"));
    const FString Root=(Preview?TEXT("/Game/Voxel/Generated/DetailPreview/"):TEXT("/Game/Voxel/Generated/Detail/"))+Run;
    const FString DiskRoot=FPackageName::LongPackageNameToFilename(Root);
    if(IFileManager::Get().DirectoryExists(*DiskRoot))return Fail(TEXT("RunId already exists; outputs are immutable"));
    TSharedPtr<FJsonObject> Publication;if(!ReadJson(Directory/TEXT("appearance/published.json"),Publication))return Fail(TEXT("invalid publication"));
    const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;if(!Publication->TryGetArrayField(TEXT("models"),Rows))return Fail(TEXT("missing published models"));
    FString Error;auto Catalog=FVoxelPublishedAppearanceCatalog::Load(Directory,Error);if(!Catalog)return Fail(Error);
    auto* Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelDetailAsset.M_VoxelDetailAsset"));if(!Base)return Fail(TEXT("missing detail material"));
    FString MaterialHash;if(!FileDigest(FPackageName::LongPackageNameToFilename(TEXT("/Game/Voxel/M_VoxelDetailAsset"),TEXT(".uasset")),MaterialHash))return Fail(TEXT("material digest unavailable"));
    FString BuilderIdentity;
    for(const TCHAR* Source:{TEXT("VoxelDetailAssetSubsystem.cpp"),TEXT("VoxelDetailMeshLOD.h"),TEXT("VoxelDetailWindBounds.h"),TEXT("VoxelAssetAppearance.cpp"),TEXT("VoxelBakeDetailMeshesCommandlet.cpp"),TEXT("VoxelMeshAttributeFingerprint.h"),TEXT("VoxelMeshAttributeFingerprint.cpp")}){
        FString H;if(!FileDigest(FPaths::ProjectDir()/TEXT("Source/VoxelEarth")/Source,H))return Fail(TEXT("builder source digest unavailable"));BuilderIdentity+=FString(Source)+TEXT(":")+H+TEXT("\n");
    }
    const bool Lods=FParse::Param(FCommandLine::Get(),TEXT("VoxelDetailMeshLOD"));
    float Tolerance=.015f,MinSaving=.20f;
    FParse::Value(FCommandLine::Get(),TEXT("VoxelDetailLodColorTolerance="),Tolerance);Tolerance=FMath::Clamp(Tolerance,0.f,.05f);
    FParse::Value(FCommandLine::Get(),TEXT("VoxelDetailLodMinSaving="),MinSaving);MinSaving=FMath::Clamp(MinSaving,0.f,1.f);
    if(!FMath::IsFinite(Tolerance)||!FMath::IsFinite(MinSaving))return Fail(TEXT("nonfinite LOD settings"));
    const FString Settings=FString::Printf(TEXT("schema=2;lod=%d;tolerance=%.9g;minSaving=%.9g;screens=1,.10,.025;patches=2,4;bounds=windXY-v1-allLOD-margin0.01UU-unitScale-quarterYaw-Z0-missing30-rejectNonFinite;collision=0;nanite=0;cpuAccess=1;fingerprint=1;authoredLOD=1"),Lods?1:0,Tolerance,MinSaving);
    const FString Engine=FEngineVersion::Current().ToString();
    const FString Host=UTF8_TO_TCHAR(FPlatformProperties::PlatformName());
    auto Manifest=MakeShared<FJsonObject>();Manifest->SetNumberField(TEXT("schema"),2);Manifest->SetNumberField(TEXT("render_attribute_fingerprint_schema"),FVoxelMeshAttributeFingerprint::SchemaVersion);Manifest->SetBoolField(TEXT("preview_only"),Preview);
    Manifest->SetStringField(TEXT("engine_version"),Engine);Manifest->SetStringField(TEXT("stage"),TEXT("uncooked-editor-source; platform cook and runtime acceptance pending"));
    Manifest->SetStringField(TEXT("settings"),Settings);Manifest->SetStringField(TEXT("builder_sha256"),TextDigest(BuilderIdentity));
    Manifest->SetStringField(TEXT("material_source_sha256"),MaterialHash);Manifest->SetStringField(TEXT("host_platform"),Host);
    FString PublicationHash;if(!FileDigest(Directory/TEXT("appearance/published.json"),PublicationHash))return Fail(TEXT("publication digest unavailable"));Manifest->SetStringField(TEXT("publication_sha256"),PublicationHash);
    IFileManager::Get().MakeDirectory(*DiskRoot,true);TArray<TSharedPtr<FJsonValue>> Models;
    TMap<FString,TSharedPtr<FJsonObject>> DerivedRows;
    for(const auto& V:*Rows){
        const auto O=V->AsObject();const FString ID=O->GetStringField(TEXT("id")),Species=O->GetStringField(TEXT("species"));
        if(!SafePart(ID)||!SafePart(Species))return Fail(TEXT("unsafe publication identity"));
        TArray<uint8> Bytes;if(!FFileHelper::LoadFileToArray(Bytes,*(Directory/TEXT("banks")/Species/(ID+TEXT(".vxa")))))return Fail(TEXT("missing VXA"));
        vxc::AssetGrid Grid;if(Grid.parse(Bytes.GetData(),size_t(Bytes.Num()))!=vxc::AssetParseError::kOk)return Fail(TEXT("invalid VXA"));
        if(Grid.onTerrainLattice()||Grid.hasParts())continue;
        const FString GeometrySHA=Digest(Bytes);if(GeometrySHA!=O->GetStringField(TEXT("geometry_sha256")))return Fail(TEXT("source changed after catalog verification"));
        if(!Preview){TSharedPtr<FJsonObject> Meta;
            if(!ReadJson(Library/Species/ID/TEXT("meta.json"),Meta))return Fail(TEXT("approved library metadata missing for ")+ID);
            bool Approved=false,Candidate=false;FString Review;
            if(!Meta->TryGetBoolField(TEXT("visual_approved"),Approved)||!Approved||!Meta->TryGetStringField(TEXT("review_status"),Review)||Review!=TEXT("endorsed")||(Meta->TryGetBoolField(TEXT("inventory_candidate"),Candidate)&&Candidate))return Fail(TEXT("source is not explicitly endorsed: ")+ID);
            FString LibrarySHA;if(!FileDigest(Library/Species/ID/TEXT("tree.vxa"),LibrarySHA)||LibrarySHA!=GeometrySHA)return Fail(TEXT("endorsed source differs from publication"));
        }
        const uint32 Resource=Catalog->FindResource(O->GetStringField(TEXT("geometry_md5")));if(!Resource)return Fail(TEXT("source appearance binding missing"));
        const auto Appearance=Catalog->Sources()[Resource].Appearance;
        const FString Key=TextDigest(GeometrySHA+TEXT("\n")+O->GetStringField(TEXT("sha256"))+TEXT("\n")+BuilderIdentity+Settings+Engine+MaterialHash+Host);
        if(const auto* Existing=DerivedRows.Find(Key)){
            auto Alias=MakeShared<FJsonObject>(**Existing);Alias->SetStringField(TEXT("id"),ID);Alias->SetStringField(TEXT("species"),Species);
            Models.Add(MakeShared<FJsonValueObject>(Alias));continue;
        }
        const FString PackageName=Root/TEXT("D_")+Key,MatPackageName=Root/TEXT("M_")+Key;
        auto* MP=CreatePackage(*MatPackageName);auto* MI=NewObject<UMaterialInstanceConstant>(MP,*FPackageName::GetLongPackageAssetName(MatPackageName),RF_Public|RF_Standalone);
        MI->SetParentEditorOnly(Base);MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TreeAppearance")),1.f);
        MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TreeNeedle")),Appearance->IsNeedle()?1.f:0.f);
        MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("FoliageCutout")),Appearance->UsesFoliageMask()?1.f:0.f);MI->PostEditChange();
        auto* P=CreatePackage(*PackageName);auto* Mesh=VoxelBakePersistentDetailMesh(Grid,Appearance,P,*FPackageName::GetLongPackageAssetName(PackageName),MI,Error);if(!Mesh)return Fail(Error);
        auto Facts=MeshFacts(Mesh,Error);if(!Facts)return Fail(TEXT("baked mesh fingerprint unavailable: ")+Error);
        FString MeshFile,MaterialFile;if(!SaveAsset(MP,MI,MatPackageName,MaterialFile)||!SaveAsset(P,Mesh,PackageName,MeshFile))return Fail(TEXT("package save failed"));
        FString MeshHash,MIHash;if(!FileDigest(MeshFile,MeshHash)||!FileDigest(MaterialFile,MIHash))return Fail(TEXT("saved package digest failed"));
        auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("id"),ID);Row->SetStringField(TEXT("species"),Species);Row->SetStringField(TEXT("derived_key"),Key);
        Row->SetStringField(TEXT("geometry_sha256"),GeometrySHA);Row->SetStringField(TEXT("appearance_sha256"),O->GetStringField(TEXT("sha256")));
        Row->SetNumberField(TEXT("voxel_pitch_um"),Grid.voxelSizeUm());Row->SetStringField(TEXT("object_path"),Mesh->GetPathName());
        Row->SetStringField(TEXT("package_file"),FPaths::ConvertRelativePathToFull(MeshFile));Row->SetStringField(TEXT("package_sha256"),MeshHash);
        Row->SetStringField(TEXT("material_object_path"),MI->GetPathName());Row->SetStringField(TEXT("material_file"),FPaths::ConvertRelativePathToFull(MaterialFile));Row->SetStringField(TEXT("material_sha256"),MIHash);
        Row->SetObjectField(TEXT("mesh_facts"),Facts);Models.Add(MakeShared<FJsonValueObject>(Row));DerivedRows.Add(Key,Row);
    }
    Manifest->SetArrayField(TEXT("models"),Models);
    if(!WriteJson(DiskRoot/TEXT("detail-bake.json"),Manifest))return Fail(TEXT("manifest save failed"));
    UE_LOG(LogTemp,Display,TEXT("DetailBake complete: %d meshes; previewOnly=%d; manifest=%s; fresh-process verification and cook still required"),Models.Num(),Preview?1:0,*(DiskRoot/TEXT("detail-bake.json")));return 0;
#endif
}
