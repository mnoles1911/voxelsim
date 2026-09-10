#include "VoxelEnvironmentAsset.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace {
FVoxelEnvironmentAssetDescriptor DescriptorFixture(){
    FVoxelEnvironmentAssetDescriptor D;D.SpecId=TEXT("composed-test-oak");D.Kind=TEXT("tree");D.Category=TEXT("environment");
    D.SourceHash=TEXT("0123456789abcdef0123456789abcdef");D.SpecHash=TEXT("spec");D.CatalogHash=TEXT("catalog");D.ProviderHash=TEXT("provider");D.SeedIndex=71;D.Fellable=true;return D;
}
void Word(TArray<uint8>& B,uint32 V){for(int I=0;I<4;++I)B.Add(uint8(V>>(I*8)));}
void Text(TArray<uint8>& B,const FString& S){Word(B,S.Len());for(TCHAR C:S)B.Add(uint8(C));}
TArray<uint8> VersionOneFixture(const FVoxelEnvironmentAssetDescriptor& D){
    TArray<uint8> B;Word(B,uint32(-1));Word(B,1);
    for(const FString* S:{&D.SpecId,&D.Kind,&D.Category,&D.SourceHash,&D.SpecHash,&D.CatalogHash,&D.ProviderHash})Text(B,*S);
    Word(B,D.SeedIndex);B.Add(D.Fellable?1:0);return B;
}
bool Save(FVoxelEnvironmentAssetDescriptor D,TArray<uint8>& B){B.Reset();FMemoryWriter W(B);return VoxelEnvironmentAsset::SerializeIdentity(W,D)&&!W.IsError();}
bool Read(const TArray<uint8>& B,FVoxelEnvironmentAssetDescriptor& D){FMemoryReader R(B);return VoxelEnvironmentAsset::SerializeIdentity(R,D)&&!R.IsError()&&R.AtEnd();}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEnvironmentDescriptorCompositionTest,"Voxel.Environment.DescriptorComposition",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelEnvironmentDescriptorCompositionTest::RunTest(const FString&){
    const auto Plain=DescriptorFixture();const auto V1=VersionOneFixture(Plain);TArray<uint8> Encoded;
    TestTrue(TEXT("uncomposed save"),Save(Plain,Encoded));TestTrue(TEXT("schema1 bytes unchanged"),Encoded==V1);
    FVoxelEnvironmentAssetDescriptor Loaded;
    for(uint8 Yaw=0;Yaw<4;++Yaw){
        auto D=Plain;D.ClippedGeometryHash=TEXT("ABCDEF0123456789ABCDEF0123456789");D.SourceYawQuarter=Yaw;
        TestTrue(TEXT("composed metadata valid"),D.IsValid());TestTrue(TEXT("schema3 save"),Save(D,Encoded));
        auto Expected=V1;Expected[4]=3;Text(Expected,D.ClippedGeometryHash);Expected.Add(Yaw);Expected.Add(0);
        TestTrue(TEXT("schema3 only appends metadata"),Expected==Encoded);
        TestTrue(TEXT("schema3 read"),Read(Encoded,Loaded));TestEqual(TEXT("original source identity retained"),Loaded.SourceHash,Plain.SourceHash);
        TestEqual(TEXT("clipped identity separate"),Loaded.ClippedGeometryHash,D.ClippedGeometryHash);
        TestEqual(TEXT("canonical yaw retained"),Loaded.SourceYawQuarter,Yaw);
        TestEqual(TEXT("seed retained"),Loaded.SeedIndex,Plain.SeedIndex);TestEqual(TEXT("provider retained"),Loaded.ProviderHash,Plain.ProviderHash);
        TArray<uint8> Again;TestTrue(TEXT("schema3 resave"),Save(Loaded,Again));TestTrue(TEXT("schema3 roundtrip bytes"),Again==Encoded);
        for(int32 Cut=V1.Num();Cut<Encoded.Num();++Cut){auto Truncated=Encoded;Truncated.SetNum(Cut);TestFalse(TEXT("truncated composition rejected"),Read(Truncated,Loaded));}
    }
    TestTrue(TEXT("v1 reads into previously composed descriptor"),Read(V1,Loaded));
    TestFalse(TEXT("v1 clears optional composition"),Loaded.HasComposition());TestEqual(TEXT("v1 clears yaw"),Loaded.SourceYawQuarter,uint8(0));
    const TCHAR* Names[]={TEXT("temperate-oak"),TEXT("granite-boulder"),TEXT("bramble-thicket"),TEXT("meadow-daisy")};
    for(int32 I=0;I<4;++I){
        auto D=FVoxelEnvironmentAssetDescriptor::Prototype(Names[I]);TestTrue(TEXT("legacy save"),Save(D,Encoded));
        TArray<uint8> Expected;Word(Expected,I);TestTrue(TEXT("legacy marker bytes unchanged"),Expected==Encoded);
        Loaded.ClippedGeometryHash=Plain.SourceHash;Loaded.SourceYawQuarter=3;
        TestTrue(TEXT("legacy read"),Read(Encoded,Loaded));TestTrue(TEXT("legacy restored"),Loaded.Legacy);
        TestFalse(TEXT("legacy clears composition"),Loaded.HasComposition());TestEqual(TEXT("legacy clears yaw"),Loaded.SourceYawQuarter,uint8(0));
        D.ClippedGeometryHash=Plain.SourceHash;TestFalse(TEXT("legacy composition rejected"),D.IsValid());TestFalse(TEXT("legacy composed save refused"),Save(D,Encoded));
    }
    for(const FString Bad:{FString::ChrN(31,'a'),FString::ChrN(33,'a'),FString::ChrN(32,'g')}){
        auto D=Plain;D.ClippedGeometryHash=Bad;TestFalse(TEXT("invalid clipped MD5 rejected"),D.IsValid());TestFalse(TEXT("invalid digest save refused"),Save(D,Encoded));
    }
    auto D=Plain;D.SourceYawQuarter=1;TestFalse(TEXT("orphan yaw rejected"),D.IsValid());
    D.ClippedGeometryHash=Plain.SourceHash;D.SourceYawQuarter=4;TestFalse(TEXT("invalid yaw rejected"),D.IsValid());
    D.SourceYawQuarter=0;Save(D,Encoded);Encoded[Encoded.Num()-2]=4;TestFalse(TEXT("serialized invalid yaw rejected"),Read(Encoded,Loaded));
    Save(D,Encoded);Encoded[Encoded.Num()-3]='g';TestFalse(TEXT("serialized invalid digest rejected"),Read(Encoded,Loaded));
    auto Unknown=V1;Unknown[4]=4;TestFalse(TEXT("unknown version rejected"),Read(Unknown,Loaded));
    auto EmptyV2=V1;EmptyV2[4]=3;Word(EmptyV2,0);EmptyV2.Add(0);EmptyV2.Add(0);TestFalse(TEXT("empty schema3 metadata rejected"),Read(EmptyV2,Loaded));
    // Main's schema2 provenance and composed schema3 must coexist.
    auto Combined=Plain;Combined.ClippedGeometryHash=Plain.SourceHash;Combined.SourceYawQuarter=3;
    FVoxelEnvironmentProductionProvenance P;
    P.Source={123,456,789,-31,42,-53,67,uint16(Plain.SeedIndex),2,3};
    P.StableId=vxc::assetObjectId(P.Source);P.CanonicalSourceHash=Plain.SourceHash;
    Combined.ProductionProvenance=P;
    TestTrue(TEXT("composed provenance saves"),Save(Combined,Encoded));
    TestTrue(TEXT("composed provenance reads"),Read(Encoded,Loaded));
    TestTrue(TEXT("both optional identities retained"),Loaded.HasComposition()&&Loaded.ProductionProvenance.IsSet());
    if(Loaded.ProductionProvenance.IsSet())TestTrue(TEXT("full source provenance retained"),Loaded.ProductionProvenance->Source==P.Source);
    TArray<uint8> BothAgain;TestTrue(TEXT("composed provenance resaves"),Save(Loaded,BothAgain));
    TestTrue(TEXT("combined bytes stable"),BothAgain==Encoded);
    auto Contradictory=Combined;Contradictory.SourceYawQuarter=2;
    TestFalse(TEXT("composition and provenance yaw must agree"),Contradictory.IsValid());
    Contradictory=Combined;Contradictory.ProductionProvenance->CanonicalSourceHash=TEXT("ffffffffffffffffffffffffffffffff");
    TestFalse(TEXT("composition and provenance canonical hash must agree"),Contradictory.IsValid());
    for(int32 Cut=V1.Num();Cut<Encoded.Num();++Cut){auto Truncated=Encoded;Truncated.SetNum(Cut);TestFalse(TEXT("truncated combined metadata refused"),Read(Truncated,Loaded));}
    return true;
}
#endif
