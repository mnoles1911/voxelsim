#include "VoxelEcologicalPlacement.h"
#include "VoxelAppearanceBankBinding.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <openssl/sha.h>

bool VoxelEcologicalPlacement::Install(const vxc::EcoPlacementConfig& Config,const vxc::AssetManifest& Manifest,
    vxc::AssetField& Field,std::vector<vxc::AssetSpecies>& SpeciesTable){
    if(!Config.valid())return false;
    std::vector<vxc::AssetDensityPolicy> Policies;
    for(const auto& P:Config.species)if(P.densitySpacingMm>0)
        Policies.push_back({P.bankId,P.densityAbundanceQ10,P.densitySpacingMm,Config.biomeMask});
    std::vector<vxc::AssetSpecies> NextTable;
    vxc::assetSpeciesTableFromManifest(Manifest,NextTable,Policies);
    auto NextField=Field;
    NextField.setSpecies(NextTable.data(),int(NextTable.size()));
    if(!NextField.setEcology(Config))return false;
    Field=std::move(NextField);SpeciesTable=std::move(NextTable);
    return true;
}

namespace {
bool Integer(const TSharedPtr<FJsonObject>& O,const TCHAR* Key,int32 Min,int32 Max,int32& Out){
    double Number=0;
    if(!O||!O->TryGetNumberField(Key,Number)||!FMath::IsFinite(Number)||Number<Min||Number>Max||Number!=FMath::FloorToDouble(Number))return false;
    Out=int32(Number);return true;
}
bool Hex(const FString& S,int32 Length){if(S.Len()!=Length)return false;for(TCHAR C:S)if(!FChar::IsHexDigit(C))return false;return true;}
bool StableID(const TSharedPtr<FJsonObject>& O,uint64& ID){
    FString Text;if(!O||!O->TryGetStringField(TEXT("stable_id"),Text)||!Hex(Text,16))return false;
    ID=FCString::Strtoui64(*Text,nullptr,16);return true;
}
bool Read(const FString& Path,FString& Text){
    const int64 Size=IFileManager::Get().FileSize(*Path);
    return Size>0&&Size<=16*1024*1024&&FFileHelper::LoadFileToString(Text,*Path);
}
bool SafeName(const FString& S){if(S.IsEmpty()||S.Len()>128)return false;for(TCHAR C:S)if(!((C>='a'&&C<='z')||(C>='0'&&C<='9')||C=='-'))return false;return true;}
}
bool VoxelEcologicalPlacement::Load(const FString& Path,const FString& Directory,
    const vxc::AssetManifest& Manifest,const vxc::AssetBankLibrary& Banks,
    const FVoxelAppearanceBankBinding& Binding,vxc::EcoPlacementConfig& Output,FString& Error){
    Output={};Error.Reset();
    auto Fail=[&](const FString& Why){Error=Why;return false;};
    FString Text;TSharedPtr<FJsonObject> Root;
    if(!Read(Path,Text)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root)||!Root)
        return Fail(TEXT("unreadable ecological placement JSON"));
    int32 Version=0;if(!Integer(Root,TEXT("schema_version"),1,1,Version))return Fail(TEXT("unsupported ecology schema"));
    if(!Integer(Root,TEXT("algorithm_version"),vxc::kEcoAlgorithmVersion,vxc::kEcoAlgorithmVersion,Version))
        return Fail(TEXT("ecology algorithm version mismatch; recompile placement rules for this engine"));
    FString PublicationHash;
    if(!Root->TryGetStringField(TEXT("publication_sha256"),PublicationHash)||!Hex(PublicationHash,64))return Fail(TEXT("missing publication identity"));
    TArray<uint8> Publication;
    const FString PublicationPath=Directory/TEXT("appearance/published.json");
    const int64 PublicationSize=IFileManager::Get().FileSize(*PublicationPath);
    uint8 Hash[32];
    if(PublicationSize<=0||PublicationSize>16*1024*1024||!FFileHelper::LoadFileToArray(Publication,*PublicationPath)||
       !SHA256(Publication.GetData(),Publication.Num(),Hash)||!BytesToHex(Hash,32).Equals(PublicationHash,ESearchCase::IgnoreCase))
        return Fail(TEXT("ecology configuration references a different publication snapshot"));
    const TArray<TSharedPtr<FJsonValue>>* Biomes=nullptr;
    if(!Root->TryGetArrayField(TEXT("biomes"),Biomes)||Biomes->Num()==0)return Fail(TEXT("missing biome scope"));
    uint16 BiomeMask=0;
    const TPair<const TCHAR*,vxc::BiomeId> KnownBiomes[]={
        {TEXT("ocean"),vxc::OCEAN},{TEXT("beach"),vxc::BEACH},
        {TEXT("grassland"),vxc::GRASSLAND},{TEXT("temperate_forest"),vxc::TEMPERATE_FOREST},
        {TEXT("rainforest"),vxc::RAINFOREST},{TEXT("desert"),vxc::DESERT},
        {TEXT("savanna"),vxc::SAVANNA},{TEXT("taiga"),vxc::TAIGA},
        {TEXT("tundra_alpine"),vxc::TUNDRA_ALPINE},{TEXT("bare_rock"),vxc::BARE_ROCK}};
    for(const auto& Value:*Biomes){FString Name;
        if(!Value||!Value->TryGetString(Name))return Fail(TEXT("invalid ecology biome scope"));
        uint16 Bit=0;
        for(const auto& Known:KnownBiomes)if(Name==Known.Key){Bit=uint16(1u<<Known.Value);break;}
        if(Bit==0||(BiomeMask&Bit)!=0)return Fail(TEXT("unknown or duplicate ecology biome scope"));
        BiomeMask|=Bit;
    }
    const TArray<TSharedPtr<FJsonValue>>* Communities=nullptr;
    if(!Root->TryGetArrayField(TEXT("communities"),Communities)||Communities->Num()<1||Communities->Num()>64)return Fail(TEXT("invalid community count"));
    TSet<FString> CommunityIDs;
    for(const auto& Value:*Communities){FString Name;const auto O=Value?Value->AsObject():nullptr;
        if(!O||!O->TryGetStringField(TEXT("id"),Name)||!SafeName(Name)||CommunityIDs.Contains(Name))return Fail(TEXT("invalid community identity"));
        CommunityIDs.Add(Name);
    }
    const TSharedPtr<FJsonObject>* Fields=nullptr;
    if(!Root->TryGetObjectField(TEXT("fields"),Fields))return Fail(TEXT("missing ecology fields"));
    vxc::EcoConfig Config;Config.communityCount=uint16(Communities->Num());int32 Ancient=0,Thicket=0;
    if(!Integer(*Fields,TEXT("community_mm"),1000,10000000,Config.communityMm)||
       !Integer(*Fields,TEXT("stand_mm"),1000,10000000,Config.standMm)||
       !Integer(*Fields,TEXT("feature_cell_mm"),1000,10000000,Config.featureCellMm)||
       !Integer(*Fields,TEXT("feature_radius_mm"),1,2500000,Config.featureRadiusMm)||
       !Integer(*Fields,TEXT("ancient_per_mille"),0,1000,Ancient)||
       !Integer(*Fields,TEXT("thicket_per_mille"),0,1000,Thicket))return Fail(TEXT("invalid ecology field values"));
    Config.ancientPerMille=uint16(Ancient);Config.thicketPerMille=uint16(Thicket);
    const TArray<TSharedPtr<FJsonValue>>* Profiles=nullptr;
    if(!Root->TryGetArrayField(TEXT("profiles"),Profiles)||Profiles->Num()<1||Profiles->Num()>4096)return Fail(TEXT("invalid ecology profiles"));
    std::vector<vxc::EcoNamedSpecies> Named;
    for(const auto& Value:*Profiles){
        const auto O=Value?Value->AsObject():nullptr;FString Name,Kind;
        vxc::EcoNamedSpecies Source;int32 Opacity=0;
        if(!O||!O->TryGetStringField(TEXT("species"),Name)||!SafeName(Name)||
           !O->TryGetStringField(TEXT("kind"),Kind)||!StableID(O,Source.placement.stableId)||
           !Integer(O,TEXT("crown_opacity_per_mille"),0,1000,Opacity))return Fail(TEXT("invalid ecological species identity/traits"));
        Source.name=TCHAR_TO_UTF8(*Name);Source.placement.tree=Kind==TEXT("tree");
        Source.placement.crownOpacityPerMille=uint16(Opacity);
        if(O->HasField(TEXT("stand_weights_per_mille"))){
            const TArray<TSharedPtr<FJsonValue>>* Weights=nullptr;
            if(!O->TryGetArrayField(TEXT("stand_weights_per_mille"),Weights)||Weights->Num()!=5)return Fail(TEXT("invalid stand weights"));
            for(int32 I=0;I<5;++I){double W=0;
                if(!(*Weights)[I]->TryGetNumber(W)||!FMath::IsFinite(W)||W<0||W>1000||W!=FMath::FloorToDouble(W))return Fail(TEXT("invalid stand weight"));
                Source.placement.standWeights[size_t(I)]=uint16(W);
            }
        }
        if(O->HasField(TEXT("ancient_only"))&&!O->TryGetBoolField(TEXT("ancient_only"),Source.placement.ancientOnly))return Fail(TEXT("invalid ancient-only flag"));
        if(O->HasField(TEXT("density_spacing_mm"))){
            int32 Spacing=0,Abundance=0;
            if(!Integer(O,TEXT("density_spacing_mm"),1,1000000,Spacing)||
               !Integer(O,TEXT("density_abundance_q10"),0,1024,Abundance))return Fail(TEXT("invalid density policy"));
            Source.placement.densitySpacingMm=Spacing;Source.placement.densityAbundanceQ10=uint16(Abundance);
        }
        if(!Source.placement.tree){
            if(Kind==TEXT("rock"))Source.kind=vxc::AssetKind::kRock;
            else if(Kind==TEXT("bush"))Source.kind=vxc::AssetKind::kBush;
            else if(Kind==TEXT("grass"))Source.kind=vxc::AssetKind::kGrass;
            else if(Kind==TEXT("reed"))Source.kind=vxc::AssetKind::kReed;
            else if(Kind==TEXT("flower"))Source.kind=vxc::AssetKind::kFlower;
            if(Kind!=TEXT("rock")&&Kind!=TEXT("bush")&&Kind!=TEXT("grass")&&Kind!=TEXT("reed")&&Kind!=TEXT("flower"))
                return Fail(TEXT("unsupported environment kind"));
            FString Role;if(!O->TryGetStringField(TEXT("cover_role"),Role))return Fail(TEXT("non-tree profile missing explicit cover role"));
            if(Role==TEXT("sun"))Source.placement.coverRole=vxc::EcoCoverRole::Sun;
            else if(Role==TEXT("shade"))Source.placement.coverRole=vxc::EcoCoverRole::Shade;
            else if(Role==TEXT("spring-woodland"))Source.placement.coverRole=vxc::EcoCoverRole::SpringWoodland;
            else if(Role==TEXT("shrub"))Source.placement.coverRole=vxc::EcoCoverRole::Shrub;
            else if(Role==TEXT("wetland"))Source.placement.coverRole=vxc::EcoCoverRole::Wetland;
            else if(Role==TEXT("shade-shrub"))Source.placement.coverRole=vxc::EcoCoverRole::ShadeShrub;
            else if(Role==TEXT("inert")&&Kind==TEXT("rock"))Source.placement.coverRole=vxc::EcoCoverRole::Inert;
            else return Fail(TEXT("unknown or incompatible cover role"));
            if(Kind==TEXT("rock")&&Source.placement.coverRole!=vxc::EcoCoverRole::Inert)return Fail(TEXT("rock profile must use inert canopy response"));
        }
        const TArray<TSharedPtr<FJsonValue>>* Weights=nullptr;
        if(!O->TryGetArrayField(TEXT("community_weights_per_mille"),Weights)||Weights->Num()!=Communities->Num())return Fail(TEXT("community weights mismatch"));
        for(const auto& W:*Weights){double N=0;if(!W||!W->TryGetNumber(N)||!FMath::IsFinite(N)||N<0||N>1000||N!=FMath::FloorToDouble(N))return Fail(TEXT("invalid community weight"));Source.placement.communityWeights.push_back(uint16(N));}
        const TArray<TSharedPtr<FJsonValue>>* Variants=nullptr;
        if(!O->TryGetArrayField(TEXT("variants"),Variants)||Variants->Num()<1||Variants->Num()>65536)return Fail(TEXT("invalid variant count"));
        for(const auto& V:*Variants){
            const auto Variant=V?V->AsObject():nullptr;FString ID,BankFile,MD5;vxc::EcoNamedVariant Entry;
            if(!Variant||!Variant->TryGetStringField(TEXT("id"),ID)||!SafeName(ID)||
               !Variant->TryGetStringField(TEXT("bank_file"),BankFile)||BankFile!=TEXT("banks/")+Name+TEXT("/")+ID+TEXT(".vxa")||
               !Variant->TryGetStringField(TEXT("geometry_md5"),MD5)||!Hex(MD5,32)||!StableID(Variant,Entry.placement.stableId)||
               !Integer(Variant,TEXT("height_mm"),1,1000000,Entry.placement.heightMm)||
               !Integer(Variant,TEXT("bounds_radius_mm"),1,100000,Entry.placement.crownMm)||
               !Integer(Variant,TEXT("trunk_exclusion_mm"),1,100000,Entry.placement.exclusionMm))return Fail(TEXT("invalid named variant/geometry"));
            if(Variant->HasField(TEXT("growth_form"))&&!Variant->HasTypedField<EJson::Null>(TEXT("growth_form"))){
                FString Form;
                if(!Variant->TryGetStringField(TEXT("growth_form"),Form))return Fail(TEXT("invalid growth form"));
                if(Form==TEXT("open"))Entry.placement.growthForm=vxc::EcoGrowthForm::Open;
                else if(Form==TEXT("woodland"))Entry.placement.growthForm=vxc::EcoGrowthForm::Woodland;
                else if(Form==TEXT("edge"))Entry.placement.growthForm=vxc::EcoGrowthForm::Edge;
                else if(Form==TEXT("compact"))Entry.placement.growthForm=vxc::EcoGrowthForm::Compact;
                else if(Form==TEXT("spreading"))Entry.placement.growthForm=vxc::EcoGrowthForm::Spreading;
                else if(Form==TEXT("leaning"))Entry.placement.growthForm=vxc::EcoGrowthForm::Leaning;
                else if(!Form.IsEmpty())return Fail(TEXT("unknown growth form"));
            }
            Entry.filename=TCHAR_TO_UTF8(*(ID+TEXT(".vxa")));Entry.sourceIdentity=TCHAR_TO_UTF8(*MD5.ToLower());
            Source.variants.push_back(std::move(Entry));
        }
        Named.push_back(std::move(Source));
    }
    const auto Catalog=Binding.SourceSnapshot();
    if(!Catalog)return Fail(TEXT("ecology requires the published appearance/source snapshot"));
    std::string Why;
    const bool Bound=vxc::ecoBindPublished(Config,BiomeMask,Named,Manifest,Banks,
        [&](const vxc::AssetGrid& Grid,const std::string& Identity){
            const auto Expected=Catalog->FindResource(UTF8_TO_TCHAR(Identity.c_str()));
            return Expected!=0&&Binding.ResourceFor(&Grid)==Expected;
        },Output,Why);
    if(!Bound)return Fail(UTF8_TO_TCHAR(Why.c_str()));
    return true;
}
