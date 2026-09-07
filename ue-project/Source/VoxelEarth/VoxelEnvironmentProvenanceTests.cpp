#include "VoxelEnvironmentAsset.h"
#include "VoxelProductionCandidatePreparation.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace {
FVoxelEnvironmentAssetDescriptor ProvenanceFixture(){
    FVoxelEnvironmentAssetDescriptor D;D.SpecId=TEXT("arbitrary-forest-log");D.Kind=TEXT("tree");D.Category=TEXT("environment");
    D.SourceHash=TEXT("11111111111111111111111111111111");D.ProviderHash=TEXT("provider-v7");D.CatalogHash=TEXT("catalog-v4");D.SeedIndex=9;D.Fellable=true;
    FVoxelEnvironmentProductionProvenance P;P.Source={123,456,789,-31,42,-53,67,9,2,3};P.StableId=vxc::assetObjectId(P.Source);P.CanonicalSourceHash=TEXT("22222222222222222222222222222222");D.ProductionProvenance=P;return D;
}
TArray<uint8> ProvenanceEncode(FVoxelEnvironmentAssetDescriptor D){TArray<uint8> Bytes;FMemoryWriter W(Bytes);if(!VoxelEnvironmentAsset::SerializeIdentity(W,D))Bytes.Reset();return Bytes;}
bool ProvenanceDecode(const TArray<uint8>& Bytes,FVoxelEnvironmentAssetDescriptor& D){FMemoryReader R(Bytes);return VoxelEnvironmentAsset::SerializeIdentity(R,D)&&!R.IsError()&&R.Tell()==R.TotalSize();}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEnvironmentProvenanceTest,"Voxel.Objects.EnvironmentProvenance",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelEnvironmentProvenanceTest::RunTest(const FString&){
    const auto Original=ProvenanceFixture();const auto Bytes=ProvenanceEncode(Original);
    TestTrue(TEXT("production schema encoded"),Bytes.Num()>100);
    FVoxelEnvironmentAssetDescriptor Loaded;
    if(!TestTrue(TEXT("production schema roundtrip"),ProvenanceDecode(Bytes,Loaded))||!Loaded.ProductionProvenance.IsSet())return false;
    TestTrue(TEXT("every provenance field preserved"),Loaded.ProductionProvenance->Source==Original.ProductionProvenance->Source);
    TestTrue(TEXT("derived stable identity preserved"),Loaded.ProductionProvenance->StableId==Original.ProductionProvenance->StableId);
    TestEqual(TEXT("canonical hash preserved independently"),Loaded.ProductionProvenance->CanonicalSourceHash,Original.ProductionProvenance->CanonicalSourceHash);
    TestEqual(TEXT("clipped hash preserved independently"),Loaded.SourceHash,Original.SourceHash);
    for(int32 N=0;N<Bytes.Num();++N){TArray<uint8> Cut;Cut.Append(Bytes.GetData(),N);FVoxelEnvironmentAssetDescriptor D; if(ProvenanceDecode(Cut,D)){AddError(FString::Printf(TEXT("Truncation accepted at %d"),N));return false;}}
    auto Unknown=Bytes;Unknown[4]=3;TestFalse(TEXT("unknown schema refused"),ProvenanceDecode(Unknown,Loaded));
    auto Bad=Original;Bad.ProductionProvenance->Source.yawQuarter=4;TestFalse(TEXT("invalid yaw refused"),Bad.IsValid());
    Bad=Original;Bad.SeedIndex=65536;TestFalse(TEXT("descriptor seed narrowing refused"),Bad.IsValid());
    Bad=Original;Bad.SeedIndex=10;TestFalse(TEXT("descriptor seed mismatch refused"),Bad.IsValid());
    Bad=Original;Bad.ProductionProvenance->StableId.low^=1;TestFalse(TEXT("stable identity mismatch refused"),Bad.IsValid());
    Bad=Original;Bad.ProductionProvenance->Source.anchorVx=(int64(1)<<30)+1;Bad.ProductionProvenance->StableId=vxc::assetObjectId(Bad.ProductionProvenance->Source);TestFalse(TEXT("anchor bound enforced independent of identity"),Bad.IsValid());
    Bad=Original;Bad.ProductionProvenance->Source.catalogFingerprint=0;TestFalse(TEXT("partial production identity refused"),Bad.IsValid());
    // Identity payload is fixed 70 bytes followed by int32 length + 32 hash
    // bytes. Corrupt serialized yaw/ID directly to exercise reader validation.
    auto Corrupt=Bytes;const int32 Payload=Bytes.Num()-106;Corrupt[Payload+53]=4;
    TestFalse(TEXT("reader rejects serialized invalid yaw"),ProvenanceDecode(Corrupt,Loaded));
    Corrupt=Bytes;Corrupt[Payload+54]^=1;TestFalse(TEXT("reader rejects serialized mismatched ID"),ProvenanceDecode(Corrupt,Loaded));
    auto Generic=Original;Generic.ProductionProvenance.Reset();const auto GenericBytes=ProvenanceEncode(Generic);
    FMemoryReader Schema(GenericBytes);int32 Marker=0;uint32 Version=0;Schema<<Marker<<Version;
    TestEqual(TEXT("generic marker unchanged"),Marker,-1);TestEqual(TEXT("generic schema unchanged"),Version,1u);
    Loaded=Original;TestTrue(TEXT("schema1 reads into reused descriptor"),ProvenanceDecode(GenericBytes,Loaded));TestFalse(TEXT("schema1 clears old optional provenance"),Loaded.ProductionProvenance.IsSet());
    TestTrue(TEXT("schema1 reencoding remains byte identical"),ProvenanceEncode(Loaded)==GenericBytes);
    const TCHAR* Names[]={TEXT("temperate-oak"),TEXT("granite-boulder"),TEXT("bramble-thicket"),TEXT("meadow-daisy")};
    for(int32 I=0;I<4;++I){TArray<uint8> Legacy;FMemoryWriter W(Legacy);int32 OldMarker=I;W<<OldMarker;Loaded=Original;TestTrue(TEXT("legacy marker reads"),ProvenanceDecode(Legacy,Loaded));TestEqual(TEXT("legacy name preserved"),Loaded.SpecId,FString(Names[I]));TestFalse(TEXT("legacy clears provenance"),Loaded.ProductionProvenance.IsSet());TestTrue(TEXT("legacy bytes unchanged"),ProvenanceEncode(Loaded)==Legacy);}
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCandidateProvenanceTest,"Voxel.Objects.CandidateProvenance",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelCandidateProvenanceTest::RunTest(const FString&){
    std::vector<uint8> Vxa;auto Word=[&](uint32 V){for(int I=0;I<4;++I)Vxa.push_back(uint8(V>>(8*I)));};
    for(uint32 V:{vxc::kVxaMagic,3u,0u,0u,0u,1u,1u,1u,100u,1u,0u,0u})Word(V);Vxa.push_back(16);Word(1);
    VoxelProductionCandidate::FWork Work;Work.Descriptor=ProvenanceFixture();Work.Provenance=Work.Descriptor.ProductionProvenance->Source;Work.CanonicalSourceHash=Work.Descriptor.ProductionProvenance->CanonicalSourceHash;Work.Descriptor.ProductionProvenance.Reset();
    if(!TestTrue(TEXT("production candidate carries provenance into staged snapshot"),VoxelProductionCandidate::BuildImmutableSnapshot(Work,Vxa)))return false;
    FMemoryReader R(Work.Dynamic);uint32 GeometryVersion=0;R<<GeometryVersion;FVoxelEnvironmentAssetDescriptor Stored;
    TestTrue(TEXT("staged production identity readable"),VoxelEnvironmentAsset::SerializeIdentity(R,Stored));
    TestTrue(TEXT("staged identity includes canonical source"),Stored.ProductionProvenance.IsSet()&&Stored.ProductionProvenance->Source==Work.Provenance);
    Work.Provenance.catalogFingerprint=0;TestFalse(TEXT("candidate rejects partial production provenance"),VoxelProductionCandidate::BuildImmutableSnapshot(Work,Vxa));
    return true;
}
#endif
