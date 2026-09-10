#include "VoxelEcologicalPlacement.h"
#include "VoxelAppearanceBankBinding.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include <set>
#include <tuple>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEcologyPublishedIntegration,
    "Voxel.Ecology.PublishedLibraryIntegration",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelEcologyPublishedIntegration::RunTest(const FString& Parameters){
    (void)Parameters;
    const FString Forge=FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()/TEXT("../asset-forge"));
    FString Directory=Forge/TEXT("out/engine");
    FString Configuration=Forge/TEXT("out/ecological-placement/placement.json");
    FString Preview;
    if(FParse::Value(FCommandLine::Get(),TEXT("VoxelEcologyTestPreview="),Preview)){
        Directory=FPaths::ConvertRelativePathToFull(Preview);
        if(!TestTrue(TEXT("explicit isolated preview marker exists"),FPaths::FileExists(Directory/TEXT("PREVIEW_ONLY.json"))))return false;
        Configuration=Directory/TEXT("placement.json");
    }
    FString Error;
    auto Catalog=FVoxelPublishedAppearanceCatalog::Load(Directory,Error);
    if(!TestTrue(*FString::Printf(TEXT("published catalog loads: %s"),*Error),Catalog.IsValid()))return false;
    AddInfo(FString::Printf(TEXT("Catalog allocation bound: %llu bytes, %d source geometries"),
        Catalog->ResourceBytes(),Catalog->Sources().Num()-1));
    FVoxelAppearanceBankBinding Binding(Catalog);
    TArray<uint8> ManifestBytes;
    if(!TestTrue(TEXT("manifest readable"),FFileHelper::LoadFileToArray(ManifestBytes,*(Directory/TEXT("species.vxm")))))return false;
    vxc::AssetManifest Manifest;
    const auto ManifestError=Manifest.parse(ManifestBytes.GetData(),size_t(ManifestBytes.Num()));
    if(!TestTrue(*FString::Printf(TEXT("manifest parses (error %d, species %s, reason %d)"),int(ManifestError),UTF8_TO_TCHAR(Manifest.misfiledName().c_str()),int(Manifest.misfiledWhy())),ManifestError==vxc::AssetManifestError::kOk))return false;
    vxc::AssetBankLibrary Banks;
    Banks.configure(&Manifest,TCHAR_TO_UTF8(*(Directory/TEXT("banks"))),
        [&](const vxc::AssetGrid& Grid,const uint8_t* Data,size_t Size){Binding.Observe(Grid,Data,Size);});
    vxc::EcoPlacementConfig Config;
    if(!TestTrue(*FString::Printf(TEXT("ecological configuration binds: %s"),*Error),
        VoxelEcologicalPlacement::Load(Configuration,Directory,Manifest,Banks,Binding,Config,Error))){AddError(Error);return false;}
    TestTrue(TEXT("published profiles present"),!Config.species.empty());
    int32 Variants=0;
    for(const auto& Species:Config.species)for(const auto& Variant:Species.variants){
        ++Variants;const auto* Grid=Banks.bankGrid(Species.bankId,Variant.seedIndex);
        TestTrue(TEXT("selected runtime slot retains verified publication"),Grid&&Binding.ResourceFor(Grid)!=0);
    }
    TestTrue(TEXT("published variants present"),Variants>0);
    std::vector<vxc::AssetSpecies> SpeciesTable;vxc::assetSpeciesTableFromManifest(Manifest,SpeciesTable);
    auto Layers=Manifest.layers();vxc::assetTightenLayerCaps(Manifest,Layers);
    vxc::AssetField Field;Field.setLayers(Layers.data(),int(Layers.size()));
    Field.setSpecies(SpeciesTable.data(),int(SpeciesTable.size()));Field.setSeed(42);Field.setBankSource(&Banks);
    if(!TestTrue(TEXT("ecology installs within production tightened bounds"),VoxelEcologicalPlacement::Install(Config,Manifest,Field,SpeciesTable))){
        for(const auto& P:Config.species){
            bool Found=false;
            for(const auto& S:SpeciesTable)if(S.bankId==P.bankId){
                Found=true;
                for(const auto& V:P.variants)if(V.heightMm>Layers[S.layer].maxHeightMm||V.crownMm>Layers[S.layer].maxRadiusMm)
                    AddError(FString::Printf(TEXT("Ecology bounds mismatch bank=%u layer=%u height=%d/%d radius=%d/%d"),P.bankId,S.layer,V.heightMm,Layers[S.layer].maxHeightMm,V.crownMm,Layers[S.layer].maxRadiusMm));
            }
            if(!Found)AddError(FString::Printf(TEXT("Ecology bank %u missing from scatter table"),P.bankId));
        }
        return false;
    }
    if(!Preview.IsEmpty()){
        // Controlled habitat fixture: a shallow wet strip crossing otherwise
        // level forest. This isolates placement from terrain baking/GPU work.
        auto Facts=[](int64_t X,int64_t){
            vxc::AssetColumnFacts F;F.known=true;F.anchorSolid=true;F.surfaceMm=100000;
            const int64_t Distance=std::abs(X)*100;
            F.distanceToWaterMm=int32_t(std::min<int64_t>(Distance,INT32_MAX));
            F.standingWaterMm=Distance<4000?200:0;
            F.twiMilli=Distance<12000?12000:5000;
            return F;
        };
        using Key=std::tuple<int64_t,int64_t,uint16_t,uint16_t,uint8_t,uint8_t>;
        auto MakeKey=[](const vxc::AssetInstance& I){return Key{I.anchorXMm,I.anchorYMm,I.bankId,I.seedIndex,I.layer,I.yawQuarter};};
        FString Csv=TEXT("world_seed,species,bank_slot,x_mm,y_mm,z_mm,layer,yaw_quarter,community,stand,target_height_per_mille\n");
        const bool TraceDecisions=FParse::Param(FCommandLine::Get(),TEXT("VoxelEcologyDecisionTrace"));
        FString DecisionCsv=TEXT("world_seed,layer,cell_x,cell_y,x_mm,y_mm,reason,known,biome,distance_water_mm,water_depth_mm,canopy_per_mille,has_context,community,stand,target_height_mm,has_instance,bank_id,bank_slot\n");
        for(uint64_t WorldSeed:{42ull,173ull,901ull}){
            Field.setSeed(WorldSeed);
            const double Start=FPlatformTime::Seconds();
            const auto Whole=Field.instancesForRect({-1280,-1280,1279,1279},Facts);
            const double Elapsed=FPlatformTime::Seconds()-Start;
            int32 Trees=0,Details=0;
            for(const auto& I:Whole){
                const auto* Profile=Config.profile(I.bankId);
                TestTrue(TEXT("generated instance belongs to ecological whitelist"),Profile!=nullptr);
                if(Profile&&Profile->tree)++Trees;else ++Details;
                vxc::EcoContext Context;
                TestTrue(TEXT("sample context resolves"),vxc::ecoContextAt(WorldSeed,I.anchorXMm,I.anchorYMm,Config.fields,Context));
                Csv+=FString::Printf(TEXT("%llu,%u,%u,%lld,%lld,%d,%u,%u,%u,%u,%u\n"),WorldSeed,I.bankId,I.seedIndex,I.anchorXMm,I.anchorYMm,I.anchorZMm,I.layer,I.yawQuarter,
                    uint32(Context.community),uint32(Context.stand),uint32(Context.targetHeightPerMille));
            }
            TestTrue(TEXT("real fixture produces trees"),Trees>0);
            TestTrue(TEXT("real fixture produces instanced understory"),Details>0);
            for(int Quadrant=0;Quadrant<4;++Quadrant){
                const int64_t X=Quadrant%2?0:-1280,Y=Quadrant/2?0:-1280;
                const auto Part=Field.instancesForRect({X,Y,X+1279,Y+1279},Facts);
                std::set<Key> Expected,Actual;
                for(const auto& I:Whole){const auto R=Layers[I.layer].maxRadiusMm;
                    if(I.anchorXMm+R>=X*100&&I.anchorXMm-R<(X+1280)*100&&
                       I.anchorYMm+R>=Y*100&&I.anchorYMm-R<(Y+1280)*100)Expected.insert(MakeKey(I));}
                for(const auto& I:Part)Actual.insert(MakeKey(I));
                TestTrue(TEXT("real-asset partition matches whole-region instance set"),Expected==Actual);
            }
            AddInfo(FString::Printf(TEXT("Controlled 256m placement, seed %llu: %d trees, %d details, whole query %.3f ms (CPU only)."),WorldSeed,Trees,Details,Elapsed*1000));
            if(TraceDecisions){
                // Small diagnostic region; do not include this optional repeat
                // query in the generation timing above or log the neighbor halo.
                for(const auto& D:Field.ecologicalDecisionsForRect({-160,-160,159,159},Facts)){
                    DecisionCsv+=FString::Printf(TEXT("%llu,%u,%lld,%lld,%lld,%lld,%s,%u,%u,%d,%d,%u,%u,%u,%u,%d,%u,%d,%d\n"),
                        WorldSeed,uint32(D.site.layer),D.site.cellX,D.site.cellY,D.site.anchorXMm,D.site.anchorYMm,
                        UTF8_TO_TCHAR(vxc::ecoDecisionReasonName(D.reason)),uint32(D.facts.known),uint32(D.facts.biome),
                        D.facts.distanceToWaterMm,D.facts.standingWaterMm,uint32(D.canopyPerMille),uint32(D.hasContext),
                        uint32(D.context.community),uint32(D.context.stand),D.targetHeightMm,uint32(D.hasInstance),
                        D.hasInstance?int32(D.instance.bankId):-1,D.hasInstance?int32(D.instance.seedIndex):-1);
                }
            }
        }
        TestTrue(TEXT("placement sample saved"),FFileHelper::SaveStringToFile(Csv,*(Directory/TEXT("placement-samples.csv"))));
        if(TraceDecisions)TestTrue(TEXT("placement decisions saved"),FFileHelper::SaveStringToFile(DecisionCsv,*(Directory/TEXT("placement-decisions.csv"))));
        // Survey authored stand/community fields independently of source
        // availability. This measures land-area frequency, not just the
        // stands that happened to select a tree in the small geometry sample.
        FString FieldCsv=TEXT("world_seed,x_mm,y_mm,community,stand,target_height_per_mille\n");
        FieldCsv.Reserve(12*1024*1024);
        for(uint64_t WorldSeed:{42ull,173ull,901ull})
            for(int64_t Y=-1020000;Y<1024000;Y+=8000)
                for(int64_t X=-1020000;X<1024000;X+=8000){
                    vxc::EcoContext Context;
                    if(!vxc::ecoContextAt(WorldSeed,X,Y,Config.fields,Context)){
                        AddError(TEXT("field survey coordinate failed"));return false;
                    }
                    FieldCsv+=FString::Printf(TEXT("%llu,%lld,%lld,%u,%u,%u\n"),WorldSeed,X,Y,
                        uint32(Context.community),uint32(Context.stand),uint32(Context.targetHeightPerMille));
                }
        TestTrue(TEXT("field survey saved"),FFileHelper::SaveStringToFile(FieldCsv,*(Directory/TEXT("field-samples.csv"))));
    }
    // Mutated fixtures are copies of configuration only. Never alter library,
    // publication, bank files or endorsement state during this integration test.
    FString Text;TSharedPtr<FJsonObject> Root;
    if(!FFileHelper::LoadFileToString(Text,*Configuration)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root))return false;
    const FString Temp=FPaths::ProjectSavedDir()/TEXT("Automation")/(TEXT("ecology-")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".json"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Temp),true);
    const auto SaveScope=[&](std::initializer_list<const TCHAR*> Names){
        TArray<TSharedPtr<FJsonValue>> Values;
        for(const auto* Name:Names)Values.Add(MakeShared<FJsonValueString>(Name));
        Root->SetArrayField(TEXT("biomes"),Values);
        FString Json;FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Json));
        return FFileHelper::SaveStringToFile(Json,*Temp);
    };
    TestTrue(TEXT("save alternate biome scope"),SaveScope({TEXT("taiga"),TEXT("grassland")}));
    TestTrue(TEXT("same placement pipeline accepts authored alternate biome scopes"),VoxelEcologicalPlacement::Load(Temp,Directory,Manifest,Banks,Binding,Config,Error));
    TestEqual(TEXT("alternate scope has exact bits"),Config.biomeMask,uint16((1u<<vxc::TAIGA)|(1u<<vxc::GRASSLAND)));
    TestTrue(TEXT("save duplicate biome scope"),SaveScope({TEXT("taiga"),TEXT("taiga")}));
    TestFalse(TEXT("duplicate scope refuses"),VoxelEcologicalPlacement::Load(Temp,Directory,Manifest,Banks,Binding,Config,Error));
    TestTrue(TEXT("save unknown biome scope"),SaveScope({TEXT("typo-biome")}));
    TestFalse(TEXT("unknown scope refuses"),VoxelEcologicalPlacement::Load(Temp,Directory,Manifest,Banks,Binding,Config,Error));
    TestTrue(TEXT("restore temperate scope"),SaveScope({TEXT("temperate_forest")}));
    Root->SetNumberField(TEXT("algorithm_version"),0);
    FString WrongAlgorithm;FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&WrongAlgorithm));
    TestTrue(TEXT("save obsolete algorithm fixture"),FFileHelper::SaveStringToFile(WrongAlgorithm,*Temp));
    TestFalse(TEXT("obsolete placement algorithm refuses"),VoxelEcologicalPlacement::Load(Temp,Directory,Manifest,Banks,Binding,Config,Error));
    Root->SetNumberField(TEXT("algorithm_version"),vxc::kEcoAlgorithmVersion);
    Root->SetStringField(TEXT("publication_sha256"),FString::ChrN(64,'0'));
    FString Bad;FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Bad));
    if(!TestTrue(TEXT("test config saved"),FFileHelper::SaveStringToFile(Bad,*Temp)))return false;
    TestFalse(TEXT("stale publication snapshot refuses configuration"),VoxelEcologicalPlacement::Load(Temp,Directory,Manifest,Banks,Binding,Config,Error));
    TestTrue(TEXT("failed load clears partial configuration"),Config.species.empty());
    IFileManager::Get().Delete(*Temp);
    AddInfo(FString::Printf(TEXT("Verified %d published variants through production bank binding and ecology loader."),Variants));
    return true;
}
#endif
