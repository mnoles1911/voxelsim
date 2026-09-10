#include "VoxelEnvironmentAsset.h"
namespace {
const TCHAR* LegacyEnvironmentNames[]={TEXT("temperate-oak"),TEXT("granite-boulder"),TEXT("bramble-thicket"),TEXT("meadow-daisy")};
const TCHAR* LegacyEnvironmentKinds[]={TEXT("tree"),TEXT("rock"),TEXT("bush"),TEXT("flower")};
bool IdentityHash(const FString& S){if(S.Len()!=32)return false;for(TCHAR C:S)if(!FChar::IsHexDigit(C))return false;return true;}
bool ProductionIdentityValid(const FVoxelEnvironmentAssetDescriptor& D){
    if(!D.ProductionProvenance.IsSet())return true;
    const auto& P=D.ProductionProvenance.GetValue();const auto& S=P.Source;
    constexpr int64 Bound=int64(1)<<30;
    if(D.Legacy||!IdentityHash(P.CanonicalSourceHash)||!S.providerFingerprint||!S.catalogFingerprint||S.yawQuarter>3||D.SeedIndex>MAX_uint16||D.SeedIndex!=S.seedIndex)return false;
    if(S.anchorVx < -Bound||S.anchorVx > Bound||S.anchorVy < -Bound||S.anchorVy > Bound||S.anchorVz < -Bound||S.anchorVz > Bound)return false;
    return P.StableId==vxc::assetObjectId(S);
}
bool SafeText(const FString& S,int32 Max,bool Empty=false){
    if(S.Len()>Max||(!Empty&&S.IsEmpty()))return false;
    for(TCHAR C:S)if(C<32||C>126)return false;return true;
}
bool Md5Text(const FString& S){
    if(S.Len()!=32)return false;
    for(TCHAR C:S)if(!((C>='0'&&C<='9')||(C>='a'&&C<='f')||(C>='A'&&C<='F')))return false;
    return true;
}
bool TextField(FArchive& Ar,FString& S,int32 Max){
    if(Ar.IsLoading()&&Ar.TotalSize()-Ar.Tell()<4)return false;
    int32 N=S.Len();Ar<<N;
    if(Ar.IsError()||N<0||N>Max||(Ar.IsLoading()&&int64(N)>Ar.TotalSize()-Ar.Tell()))return false;
    if(Ar.IsLoading()){TArray<ANSICHAR> Data;Data.SetNumZeroed(N+1);Ar.Serialize(Data.GetData(),N);for(int I=0;I<N;++I)if(Data[I]<32||Data[I]>126)return false;S=ANSI_TO_TCHAR(Data.GetData());}
    else {if(!SafeText(S,Max,true))return false;FTCHARToUTF8 Data(*S);Ar.Serialize(const_cast<ANSICHAR*>(Data.Get()),N);}
    return !Ar.IsError();
}
}
bool FVoxelEnvironmentAssetDescriptor::IsValid() const {
    if(!SafeText(SpecId,128)||!SafeText(Kind,64)||Category!=TEXT("environment"))return false;
    if(!SafeText(SpecHash,128,true)||!SafeText(CatalogHash,128,true)||!SafeText(ProviderHash,128,true))return false;
    if(SourceYawQuarter>3||(!HasComposition()&&SourceYawQuarter!=0))return false;
    if(HasComposition()&&(Legacy||!Md5Text(ClippedGeometryHash)))return false;
    if(!ProductionIdentityValid(*this))return false;
    if(HasComposition()&&ProductionProvenance.IsSet()&&
       (ProductionProvenance->Source.yawQuarter!=SourceYawQuarter||
        !ProductionProvenance->CanonicalSourceHash.Equals(SourceHash,ESearchCase::IgnoreCase)))return false;
    if(Legacy){for(const auto Name:LegacyEnvironmentNames)if(SpecId==Name)return true;return false;}
    return Md5Text(SourceHash);
}
FVoxelEnvironmentAssetDescriptor FVoxelEnvironmentAssetDescriptor::Prototype(const FString& Name){
    FVoxelEnvironmentAssetDescriptor D;
    for(int I=0;I<4;++I)if(Name==LegacyEnvironmentNames[I]){D.SpecId=Name;D.Kind=LegacyEnvironmentKinds[I];D.Category=TEXT("environment");D.Fellable=I==0;D.Legacy=true;break;}return D;
}
bool VoxelEnvironmentAsset::SerializeIdentity(FArchive& Ar,FVoxelEnvironmentAssetDescriptor& D){
    if(Ar.IsLoading())D=FVoxelEnvironmentAssetDescriptor{};
    int32 Marker=-1;
    if(Ar.IsSaving()){
        if(!D.IsValid())return false;
        if(D.Legacy)for(int I=0;I<4;++I)if(D.SpecId==LegacyEnvironmentNames[I])Marker=I;
    }
    if(Ar.IsLoading()&&Ar.TotalSize()-Ar.Tell()<4)return false;
    Ar<<Marker;if(Ar.IsError())return false;
    if(Marker>=0&&Marker<4){if(Ar.IsLoading())D=FVoxelEnvironmentAssetDescriptor::Prototype(LegacyEnvironmentNames[Marker]);return true;}
    if(Marker!=-1)return false;
    if(Ar.IsLoading()&&Ar.TotalSize()-Ar.Tell()<4)return false;
    uint32 Version=D.HasComposition()?3u:(D.ProductionProvenance.IsSet()?2u:1u);
    Ar<<Version;if((Version<1||Version>3)||Ar.IsError())return false;
    if(!TextField(Ar,D.SpecId,128)||!TextField(Ar,D.Kind,64)||!TextField(Ar,D.Category,64)||!TextField(Ar,D.SourceHash,32)||
       !TextField(Ar,D.SpecHash,128)||!TextField(Ar,D.CatalogHash,128)||!TextField(Ar,D.ProviderHash,128))return false;
    if(Ar.IsLoading()&&Ar.TotalSize()-Ar.Tell()<5)return false;
    uint8 Flags=D.Fellable?1:0;Ar<<D.SeedIndex<<Flags;if(Flags>1||Ar.IsError())return false;
    D.Fellable=Flags!=0;D.Legacy=false;
    uint8 HasProvenance=Version==2?1:0;
    if(Version==3){
        if(!TextField(Ar,D.ClippedGeometryHash,32))return false;
        if(Ar.IsLoading()&&Ar.TotalSize()-Ar.Tell()<2)return false;
        if(Ar.IsSaving())HasProvenance=D.ProductionProvenance.IsSet()?1:0;
        Ar<<D.SourceYawQuarter<<HasProvenance;
        if(Ar.IsError()||D.ClippedGeometryHash.IsEmpty()||HasProvenance>1)return false;
    }
    if(HasProvenance){
        if(Ar.IsLoading()&&Ar.TotalSize()-Ar.Tell()<74)return false;
        if(Ar.IsLoading())D.ProductionProvenance.Emplace();
        auto& P=D.ProductionProvenance.GetValue();auto& S=P.Source;
        Ar<<S.worldSeed<<S.providerFingerprint<<S.catalogFingerprint;
        Ar<<S.anchorVx<<S.anchorVy<<S.anchorVz;
        Ar<<S.bankId<<S.seedIndex<<S.layer<<S.yawQuarter;
        Ar<<P.StableId.low<<P.StableId.high;
        if(Ar.IsError()||!TextField(Ar,P.CanonicalSourceHash,32))return false;
    }
    return !Ar.IsError()&&D.IsValid();
}
int32 VoxelEnvironmentAsset::QuarterYaw(const FTransform& T){
    if(!T.IsValid()||!T.GetScale3D().Equals(FVector::OneVector,1.e-8))return INDEX_NONE;
    for(int32 I=0;I<4;++I)if(T.GetRotation().Equals(FQuat(FVector::UpVector,I*UE_DOUBLE_PI*.5),1.e-8))return I;
    return INDEX_NONE;
}
bool VoxelEnvironmentAsset::IsSupportedTransform(const FTransform& T){return QuarterYaw(T)!=INDEX_NONE;}
FVector VoxelEnvironmentAsset::LocalVector(const FTransform& T,const FVector& V){
    const int32 Q=QuarterYaw(T);check(Q!=INDEX_NONE);
    switch(Q){case 1:return FVector(V.Y,-V.X,V.Z);case 2:return FVector(-V.X,-V.Y,V.Z);case 3:return FVector(-V.Y,V.X,V.Z);default:return V;}
}
FVector VoxelEnvironmentAsset::LocalPosition(const FTransform& T,const FVector& P){return LocalVector(T,P-T.GetLocation());}
FVector VoxelEnvironmentAsset::WorldPosition(const FTransform& T,const FVector& V){
    const int32 Q=QuarterYaw(T);check(Q!=INDEX_NONE);FVector P=V;
    switch(Q){case 1:P=FVector(-V.Y,V.X,V.Z);break;case 2:P=FVector(-V.X,-V.Y,V.Z);break;case 3:P=FVector(V.Y,-V.X,V.Z);break;default:break;}
    return T.GetLocation()+P;
}
