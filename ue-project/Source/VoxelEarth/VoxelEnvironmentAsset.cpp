#include "VoxelEnvironmentAsset.h"
namespace {
const TCHAR* LegacyEnvironmentNames[]={TEXT("temperate-oak"),TEXT("granite-boulder"),TEXT("bramble-thicket"),TEXT("meadow-daisy")};
const TCHAR* LegacyEnvironmentKinds[]={TEXT("tree"),TEXT("rock"),TEXT("bush"),TEXT("flower")};
bool SafeText(const FString& S,int32 Max,bool Empty=false){
    if(S.Len()>Max||(!Empty&&S.IsEmpty()))return false;
    for(TCHAR C:S)if(C<32||C>126)return false;return true;
}
bool TextField(FArchive& Ar,FString& S,int32 Max){
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
    if(Legacy){for(const auto Name:LegacyEnvironmentNames)if(SpecId==Name)return true;return false;}
    if(SourceHash.Len()!=32)return false;for(TCHAR C:SourceHash)if(!FChar::IsHexDigit(C))return false;return true;
}
FVoxelEnvironmentAssetDescriptor FVoxelEnvironmentAssetDescriptor::Prototype(const FString& Name){
    FVoxelEnvironmentAssetDescriptor D;
    for(int I=0;I<4;++I)if(Name==LegacyEnvironmentNames[I]){D.SpecId=Name;D.Kind=LegacyEnvironmentKinds[I];D.Category=TEXT("environment");D.Fellable=I==0;D.Legacy=true;break;}return D;
}
bool VoxelEnvironmentAsset::SerializeIdentity(FArchive& Ar,FVoxelEnvironmentAssetDescriptor& D){
    int32 Marker=-1;
    if(Ar.IsSaving()){
        if(!D.IsValid())return false;
        if(D.Legacy)for(int I=0;I<4;++I)if(D.SpecId==LegacyEnvironmentNames[I])Marker=I;
    }
    Ar<<Marker;if(Ar.IsError())return false;
    if(Marker>=0&&Marker<4){if(Ar.IsLoading())D=FVoxelEnvironmentAssetDescriptor::Prototype(LegacyEnvironmentNames[Marker]);return true;}
    if(Marker!=-1)return false;
    uint32 Version=1;Ar<<Version;if(Version!=1||Ar.IsError())return false;
    if(!TextField(Ar,D.SpecId,128)||!TextField(Ar,D.Kind,64)||!TextField(Ar,D.Category,64)||!TextField(Ar,D.SourceHash,32)||
       !TextField(Ar,D.SpecHash,128)||!TextField(Ar,D.CatalogHash,128)||!TextField(Ar,D.ProviderHash,128))return false;
    uint8 Flags=D.Fellable?1:0;Ar<<D.SeedIndex<<Flags;if(Flags>1||Ar.IsError())return false;
    D.Fellable=Flags!=0;D.Legacy=false;return D.IsValid();
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
