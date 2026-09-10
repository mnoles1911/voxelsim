#include "VoxelEnvironmentAuthorityIdentity.h"
#include "VoxelProductionCandidatePreparation.h"
#include "Misc/AutomationTest.h"
#include "Misc/SecureHash.h"
#include "../../../voxel-core/tests/asset_manifest_testutil.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace {
vxc::AssetGrid AuthorityIdentityGrid(bool Split=false) {
    std::vector<uint8> Bytes;auto Word=[&](uint32 V){for(int I=0;I<4;++I)Bytes.push_back(uint8(V>>(8*I)));};
    for(uint32 V:{vxc::kVxaMagic,3u,0u,0u,0u,2u,2u,2u,100u,Split?2u:1u,0u,0u})Word(V);
    Bytes.push_back(16);Word(Split?4:8);if(Split){Bytes.push_back(16);Word(4);}
    vxc::AssetGrid Grid;Grid.parse(Bytes);return Grid;
}
struct FAuthorityIdentityBank final:vxc::IAssetBankSource {
    const vxc::AssetGrid* Grid=nullptr;
    const vxc::AssetGrid* bankGrid(uint16,uint16) const override{return Grid;}
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEnvironmentAuthorityIdentityTest,"Voxel.Objects.EnvironmentAuthorityIdentity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelEnvironmentAuthorityIdentityTest::RunTest(const FString&) {
    vxmtest::VxmSpecies Spec;for(auto& W:Spec.weights)W=1000;
    const auto Raw=vxmtest::buildVxm({Spec});TArray<uint8> Bytes;Bytes.Append(Raw.data(),int32(Raw.size()));
    vxc::AssetManifest Manifest;TestTrue(TEXT("fixture parses"),Manifest.parse(Raw)==vxc::AssetManifestError::kOk);
    auto Layers=Manifest.layers();vxc::assetTightenLayerCaps(Manifest,Layers);
    std::vector<vxc::AssetSpecies> Species;vxc::assetSpeciesTableFromManifest(Manifest,Species);
    auto Grid=AuthorityIdentityGrid();FAuthorityIdentityBank Bank;Bank.Grid=&Grid;
    vxc::AssetField Field;Field.setSeed(42);Field.setLayers(Layers.data(),int(Layers.size()));Field.setSpecies(Species.data(),int(Species.size()));Field.setBankSource(&Bank);
    FString Error;auto Identity=VoxelEnvironmentAuthority::FIdentity::Create(Field,Bytes,Error);
    TestTrue(TEXT("actual derived field registered"),Identity.IsValid());if(!Identity)return false;
    const FString ExpectedCatalog=FMD5::HashBytes(Bytes.GetData(),Bytes.Num());
    TestEqual(TEXT("catalog is exact loaded manifest MD5"),FString(UTF8_TO_TCHAR(Identity->catalogIdentity(Field).c_str())),ExpectedCatalog);
    TestEqual(TEXT("canonical hash delegates production function"),FString(UTF8_TO_TCHAR(Identity->contentHash(Grid).c_str())),VoxelProductionCandidate::GridContentHash(Grid));
    TestEqual(TEXT("independent canonical byte digest"),FString(UTF8_TO_TCHAR(Identity->contentHash(Grid).c_str())),FString(TEXT("dcfc6f3410eed22ccfa8e6bc1e545338")));
    TestEqual(TEXT("run compression does not alter canonical hash"),FString(UTF8_TO_TCHAR(Identity->contentHash(AuthorityIdentityGrid(true)).c_str())),FString(UTF8_TO_TCHAR(Identity->contentHash(Grid).c_str())));
    vxc::AssetGrid Invalid;TestTrue(TEXT("invalid grid refused"),Identity->contentHash(Invalid).empty());
    vxc::AssetField Other;Other.setSeed(42);Other.setLayers(Layers.data(),int(Layers.size()));Other.setSpecies(Species.data(),int(Species.size()));Other.setBankSource(&Bank);
    TestTrue(TEXT("same tables different unregistered field refused"),Identity->catalogIdentity(Other).empty());
    Field.setSeed(43);TestTrue(TEXT("mutated seed refused"),Identity->catalogIdentity(Field).empty());Field.setSeed(42);
    auto Changed=Layers;Changed[0].densityPerMille^=1;Field.setLayers(Changed.data(),int(Changed.size()));
    TestTrue(TEXT("mutated live table refused"),Identity->catalogIdentity(Field).empty());
    TestFalse(TEXT("wrong field cannot register under supplied manifest"),VoxelEnvironmentAuthority::FIdentity::Create(Field,Bytes,Error).IsValid());
    Field.setLayers(Layers.data(),int(Layers.size()));TestFalse(TEXT("truncated manifest refused"),VoxelEnvironmentAuthority::FIdentity::Create(Field,TConstArrayView<uint8>(Bytes.GetData(),4),Error).IsValid());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEnvironmentAuthorityProviderReplayTest,"Voxel.Objects.EnvironmentAuthorityProviderReplay",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelEnvironmentAuthorityProviderReplayTest::RunTest(const FString&) {
    vxc::SyntheticTileSampler Tiles(42);vxc::World<16> World(42,Tiles,"active-fine-provider");
    vxc::EditLog Legacy(42,16);Legacy.append({0,0,0},{{0,vxc::MAT_ROCK}});
    TestTrue(TEXT("legacy unstamped accepted explicitly"),World.replay(Legacy,World.log().providerId()));
    TestTrue(TEXT("legacy classified unstamped"),World.lastProviderCheck()==vxc::EditLog::ProviderCheck::kUnstamped);
    TestTrue(TEXT("new stream retains live provider stamp"),World.log().providerId()=="active-fine-provider");
    const auto Before=World.editedDigest();vxc::EditLog Wrong(42,16,"other-provider");Wrong.append({1,0,0},{{0,vxc::MAT_ROCK}});
    TestFalse(TEXT("same seed wrong stamped provider refused"),World.replay(Wrong,World.log().providerId()));
    TestTrue(TEXT("provider mismatch classified"),World.lastProviderCheck()==vxc::EditLog::ProviderCheck::kMismatch);
    TestEqual(TEXT("refusal preserves world"),World.editedDigest(),Before);
    vxc::EditLog Matching(42,16,"active-fine-provider");Matching.append({1,0,0},{{0,vxc::MAT_ROCK}});
    TestTrue(TEXT("matching stamped provider accepted"),World.replay(Matching,World.log().providerId()));
    vxc::EditLog WrongSeed(43,16,"active-fine-provider");TestFalse(TEXT("seed mismatch remains refused"),World.replay(WrongSeed,World.log().providerId()));
    std::vector<uint8> Encoded;Matching.serialize(Encoded);Encoded[8]^=1;
    TestFalse(TEXT("worldgen mismatch still fails parser"),vxc::EditLog::parse(Encoded.data(),Encoded.size()).has_value());
    return true;
}
#endif
