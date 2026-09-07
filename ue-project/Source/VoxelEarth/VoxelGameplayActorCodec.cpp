#include "VoxelGameplayActors.h"
#include "VoxelInventoryComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace VoxelGameplayActors
{
namespace Codec
{
bool ValidVector(const FVector& V,double Limit)
{
    return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z) &&
        V.GetAbsMax()<=Limit;
}
bool Range(double V,double Min,double Max) { return FMath::IsFinite(V) && V>=Min && V<=Max; }
bool Valid(const FRecord& R)
{
    const auto Scale=R.Transform.GetScale3D(); const auto Q=R.Transform.GetRotation();
    if(!R.Id.IsValid() || !ValidVector(R.Transform.GetLocation(),1e12) || !ValidVector(R.Velocity,1e7) ||
        !ValidVector(R.AngularVelocity,1e5) || !ValidVector(Scale,1000) || Scale.GetMin()<=0 ||
        Q.ContainsNaN() || !Q.IsNormalized() || R.AssetName.Len()>256 || R.AssetPath.Len()>2048 ||
        !Range(R.Remaining,R.Kind==EKind::Item?-1:0,86400) || !Range(R.FlightAge,0,1e9) || !Range(R.SettledAge,0,1e9) ||
        !Range(R.Throttle,-1,1) || !Range(R.Radius,0,1e5)) return false;
    if(R.Kind==EKind::Boat || R.Kind==EKind::Glider)
    {
        // Missing authored content cannot silently become a differently sized hull.
        if(R.AssetName.IsEmpty() || R.AssetHash.Len()!=64 || R.AssetPath.IsEmpty() || !R.Item.IsNone() || R.Count!=0) return false;
        for(TCHAR C:R.AssetHash) if(!FChar::IsHexDigit(C)) return false;
        return true;
    }
    if(!R.AssetName.IsEmpty() || !R.AssetPath.IsEmpty() || !R.AssetHash.IsEmpty()) return false;
    if(R.Kind==EKind::Explosive) return R.Item.IsNone() && R.Count==0 && R.Radius>=175 && R.Radius<=225 && R.Remaining<=3;
    if(R.Kind!=EKind::Item || !R.Owner.IsValid() || R.Item.IsNone() || R.Count<=0) return false;
    TArray<FVoxelInventorySlot> Slots; FVoxelInventorySlot Slot; Slot.ItemId=R.Item; Slot.Count=R.Count; Slots.Add(Slot);
    return UVoxelInventoryComponent::ValidateSnapshot(Slots,0);
}
TArray<TSharedPtr<FJsonValue>> Vector(const FVector& V)
{
    return {MakeShared<FJsonValueNumber>(V.X),MakeShared<FJsonValueNumber>(V.Y),MakeShared<FJsonValueNumber>(V.Z)};
}
bool Vector(const FJsonObject& J,const TCHAR* Key,FVector& V)
{
    const TArray<TSharedPtr<FJsonValue>>* A=nullptr;
    if(!J.TryGetArrayField(Key,A) || A->Num()!=3) return false;
    for(int32 I=0;I<3;++I) if(!(*A)[I]->TryGetNumber(V[I])) return false;
    return true;
}
}
bool Encode(const TArray<FRecord>& Records,TArray<uint8>& Bytes)
{
    Bytes.Reset(); if(Records.Num()>8192) return false;
    TSet<FGuid> Ids; TArray<TSharedPtr<FJsonValue>> Array;
    for(const auto& R:Records)
    {
        if(!Codec::Valid(R) || Ids.Contains(R.Id)) return false;
        Ids.Add(R.Id); auto J=MakeShared<FJsonObject>();
        J->SetStringField(TEXT("id"),R.Id.ToString(EGuidFormats::Digits));
        J->SetStringField(TEXT("owner"),R.Owner.ToString(EGuidFormats::Digits));
        J->SetNumberField(TEXT("kind"),uint8(R.Kind));
        J->SetArrayField(TEXT("position"),Codec::Vector(R.Transform.GetLocation()));
        J->SetArrayField(TEXT("scale"),Codec::Vector(R.Transform.GetScale3D()));
        const auto Q=R.Transform.GetRotation();
        J->SetArrayField(TEXT("rotation"),{MakeShared<FJsonValueNumber>(Q.X),MakeShared<FJsonValueNumber>(Q.Y),MakeShared<FJsonValueNumber>(Q.Z),MakeShared<FJsonValueNumber>(Q.W)});
        J->SetArrayField(TEXT("velocity"),Codec::Vector(R.Velocity));
        J->SetArrayField(TEXT("angularVelocity"),Codec::Vector(R.AngularVelocity));
        J->SetStringField(TEXT("asset"),R.AssetName); J->SetStringField(TEXT("path"),R.AssetPath); J->SetStringField(TEXT("hash"),R.AssetHash);
        J->SetStringField(TEXT("item"),R.Item.IsNone()?FString():R.Item.ToString()); J->SetNumberField(TEXT("count"),R.Count);
        J->SetBoolField(TEXT("resting"),R.Resting); J->SetBoolField(TEXT("grounded"),R.Grounded); J->SetBoolField(TEXT("launchPending"),R.LaunchPending);
        J->SetNumberField(TEXT("remaining"),R.Remaining); J->SetNumberField(TEXT("flightAge"),R.FlightAge); J->SetNumberField(TEXT("settledAge"),R.SettledAge);
        J->SetNumberField(TEXT("throttle"),R.Throttle); J->SetNumberField(TEXT("radius"),R.Radius);
        Array.Add(MakeShared<FJsonValueObject>(J));
    }
    auto Root=MakeShared<FJsonObject>(); Root->SetNumberField(TEXT("version"),1); Root->SetArrayField(TEXT("actors"),Array);
    FString Text; if(!FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&Text))) return false;
    FTCHARToUTF8 Utf8(*Text); if(Utf8.Length()>16*1024*1024) return false;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()); return true;
}
bool Decode(const TArray<uint8>& Bytes,TArray<FRecord>& Records)
{
    Records.Reset(); if(Bytes.IsEmpty() || Bytes.Num()>16*1024*1024) return false;
    FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Bytes.Num());
    FString Text(Utf8.Length(),Utf8.Get()); TSharedPtr<FJsonObject> Root; double Version=0;
    const TArray<TSharedPtr<FJsonValue>>* Array=nullptr;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root) || !Root || Root->Values.Num()!=2 ||
        !Root->TryGetNumberField(TEXT("version"),Version) || Version!=1 || !Root->TryGetArrayField(TEXT("actors"),Array) || Array->Num()>8192) return false;
    TArray<FRecord> Staged; TSet<FGuid> Ids;
    for(const auto& Value:*Array)
    {
        const TSharedPtr<FJsonObject>* Ptr=nullptr;
        if(!Value->TryGetObject(Ptr) || !Ptr || !Ptr->IsValid()) return false;
        const auto& J=**Ptr; FRecord R; FString Id,Owner,Item; double Kind=0,Count=0;
        FVector Position,Scale; FQuat Q; const TArray<TSharedPtr<FJsonValue>>* Rotation=nullptr;
        if(J.Values.Num()!=21 || !J.TryGetStringField(TEXT("id"),Id) || !FGuid::ParseExact(Id,EGuidFormats::Digits,R.Id) ||
            !J.TryGetStringField(TEXT("owner"),Owner) || !FGuid::ParseExact(Owner,EGuidFormats::Digits,R.Owner) ||
            !J.TryGetNumberField(TEXT("kind"),Kind) || Kind<1 || Kind>4 || Kind!=FMath::FloorToDouble(Kind) ||
            !Codec::Vector(J,TEXT("position"),Position) || !Codec::Vector(J,TEXT("scale"),Scale) ||
            !Codec::Vector(J,TEXT("velocity"),R.Velocity) || !Codec::Vector(J,TEXT("angularVelocity"),R.AngularVelocity) ||
            !J.TryGetArrayField(TEXT("rotation"),Rotation) || Rotation->Num()!=4 ||
            !J.TryGetStringField(TEXT("asset"),R.AssetName) || !J.TryGetStringField(TEXT("path"),R.AssetPath) || !J.TryGetStringField(TEXT("hash"),R.AssetHash) ||
            !J.TryGetStringField(TEXT("item"),Item) || Item.Len()>128 || !J.TryGetNumberField(TEXT("count"),Count) ||
            !Codec::Range(Count,0,MAX_int32) || Count!=FMath::FloorToDouble(Count) ||
            !J.TryGetBoolField(TEXT("resting"),R.Resting) || !J.TryGetBoolField(TEXT("grounded"),R.Grounded) || !J.TryGetBoolField(TEXT("launchPending"),R.LaunchPending) ||
            !J.TryGetNumberField(TEXT("remaining"),R.Remaining) || !J.TryGetNumberField(TEXT("flightAge"),R.FlightAge) ||
            !J.TryGetNumberField(TEXT("settledAge"),R.SettledAge) || !J.TryGetNumberField(TEXT("throttle"),R.Throttle) || !J.TryGetNumberField(TEXT("radius"),R.Radius)) return false;
        double* Components[]={&Q.X,&Q.Y,&Q.Z,&Q.W};
        for(int32 I=0;I<4;++I) if(!(*Rotation)[I]->TryGetNumber(*Components[I])) return false;
        // Validate before FTransform's rotation diagnostics can consume untrusted data.
        if(Q.ContainsNaN() || !Q.IsNormalized() || !Codec::ValidVector(Position,1e12) || !Codec::ValidVector(Scale,1000) || Scale.GetMin()<=0) return false;
        R.Kind=EKind(uint8(Kind)); R.Count=int32(Count); R.Item=Item.IsEmpty()?NAME_None:FName(*Item);
        R.Transform=FTransform(Q,Position,Scale);
        if(!Codec::Valid(R) || Ids.Contains(R.Id)) return false;
        Ids.Add(R.Id); Staged.Add(MoveTemp(R));
    }
    Records=MoveTemp(Staged); return true;
}
}
