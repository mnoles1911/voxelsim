#include "VoxelTreeFellingPrototype.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "Async/Async.h"
#include "VoxelDebrisLifecycle.h"
#include "VoxelDetachedPersistence.h"
#include "VoxelPackedTimberMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelFineTileStreamer.h"
#include "VoxelEarth.h"
#include "Components/BoxComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "VoxelEarthPlayerController.h"

namespace {
// Section metadata byte: low two bits are the existing fracture tag;
// Bits2/3 preserve appearance/needle mode; bit4 explicitly disables cutouts.
// Legacy zero bit4 retains the existing cutout behavior.
bool ValidAppearanceFlags(uint8 Flags){return (Flags&~31)==0&&(Flags&3)<=2&&(!(Flags&8)||(Flags&4))&&(!(Flags&16)||(Flags&4));}
uint8 AppearanceFlags(UProceduralMeshComponent* C,uint8 Cap){
    float Appearance=0,Needle=0,Cutout=1;
    if(auto M=C->GetMaterial(0)){
        M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("TreeAppearance")),Appearance);
        M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("TreeNeedle")),Needle);
        M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("FoliageCutout")),Cutout);
    }
    return Cap|(Appearance>.5f?4:0)|(Appearance>.5f&&Needle>.5f?8:0)|(Appearance>.5f&&Cutout<.5f?16:0);
}
void RestoreAppearanceFlags(UMaterialInstanceDynamic* M,uint8 Flags){
    M->SetScalarParameterValue(TEXT("TreeAppearance"),(Flags&4)?1.f:0.f);
    M->SetScalarParameterValue(TEXT("TreeNeedle"),(Flags&8)?1.f:0.f);
    M->SetScalarParameterValue(TEXT("FoliageCutout"),(Flags&16)?0.f:1.f);
}
}

struct FTimberStagedRestore {
    struct FSection {FProcMeshSection Mesh;FTransform Relative;bool Visible=true;uint8 Cap=0;};
    TAtomic<bool> Cancelled{false};
    FVoxelImmutableGeometry Geometry;
    TFunction<void(bool)> Completion;
    TArray<FSection> Sections;
    FBox Trunk;
    FTransform Transform,HingeTransform;
    FVector Linear=FVector::ZeroVector,Angular=FVector::ZeroVector,Heading=FVector::ForwardVector;
    FVoxelDebrisLifetimeState Lifetime;
    double Age=0;
    int32 Next=0;
    bool Hinged=false,Awake=false,Breakable=false,Ready=false,Valid=false,Adopted=false;
};

AVoxelFallingTimber::AVoxelFallingTimber(){
    PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickGroup=TG_PrePhysics;
    Cleanup=CreateDefaultSubobject<UVoxelDebrisLifecycle>(TEXT("Cleanup"));
    Cleanup->Configure(EVoxelDebrisLifetime::Substantial);
    Body=CreateDefaultSubobject<UBoxComponent>(TEXT("TimberPhysics"));SetRootComponent(Body);
    Body->SetMobility(EComponentMobility::Movable);
    Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Body->SetCollisionObjectType(ECC_PhysicsBody);
    Body->SetCollisionResponseToAllChannels(ECR_Ignore);
    Body->SetCollisionResponseToChannel(ECC_WorldStatic,ECR_Block);
    Body->SetCollisionResponseToChannel(ECC_WorldDynamic,ECR_Block);
    Body->SetNotifyRigidBodyCollision(true);Body->BodyInstance.bUseCCD=true;
    Body->SetLinearDamping(.15f);Body->SetAngularDamping(.08f);
    Body->OnComponentHit.AddDynamic(this,&AVoxelFallingTimber::Contact);
}
void AVoxelFallingTimber::Initialize(const TArray<UProceduralMeshComponent*>& Meshes,const FBox& LocalTrunk,
                                    const FTransform& Source,const FVector& Direction,bool CanBreak){
    if(StagedRestore||PreparedRestore){CancelStagedObjectRestore();return;}
    GeometrySnapshot.Reset();
    Trunk=LocalTrunk;Breakable=CanBreak;
    SetActorTransform(FTransform(Source.GetRotation(),Source.TransformPosition(Trunk.GetCenter())));
    Body->SetBoxExtent(Trunk.GetExtent().ComponentMax(FVector(5)));
    for(auto C:Meshes){
        if(C->GetOwner()!=this){
            // UActorComponent::PostRename updates both actors' OwnedComponents
            // arrays. Attachment alone leaves the stump owning these meshes,
            // and unloading it would destroy an independently moving log.
            const FName OwnedName=MakeUniqueObjectName(this,C->GetClass(),C->GetFName());
            const bool Transferred=C->Rename(*OwnedName.ToString(),this,REN_DontCreateRedirectors|REN_NonTransactional);
            checkf(Transferred&&C->GetOwner()==this,TEXT("Timber mesh ownership transfer failed"));
            if(!Transferred)return;
        }
        C->AttachToComponent(Body,FAttachmentTransformRules::KeepWorldTransform);
        C->SetVisibility(!CanBreak||(!C->ComponentHasTag(TEXT("FractureLowerCap"))&&!C->ComponentHasTag(TEXT("FractureUpperCap"))));Visuals.Add(C);
    }
    if(CanBreak){
        FallHeading=Direction.GetSafeNormal2D();if(FallHeading.IsNearlyZero())FallHeading=FVector::ForwardVector;
        const FVector Axis=FVector::CrossProduct(FVector::UpVector,FallHeading).GetSafeNormal();
        const FVector Pivot=Source.TransformPosition(FVector(Trunk.GetCenter().X,Trunk.GetCenter().Y,Trunk.Min.Z));
        const FQuat Lean(Axis,FMath::DegreesToRadians(1.5));
        SetActorTransform(FTransform(Lean*Source.GetRotation(),Pivot+Lean.RotateVector(Source.TransformPosition(Trunk.GetCenter())-Pivot)));
    }
    Body->SetSimulatePhysics(true);
    const double VolumeM3=Trunk.GetVolume()/1000000.;
    Body->SetMassOverrideInKg(NAME_None,float(FMath::Max(5.,VolumeM3*550.)),true);
    Body->SetMaxDepenetrationVelocity(NAME_None,200.f);
    Body->SetPhysicsMaxAngularVelocityInDegrees(90.f);
    Body->BodyInstance.SetPositionSolverIterationCount(16);Body->BodyInstance.SetVelocitySolverIterationCount(8);
    auto Wood=NewObject<UPhysicalMaterial>(this);Wood->Friction=.8f;Wood->Restitution=0;
    if(CanBreak){Wood->SleepLinearVelocityThreshold=0;Wood->SleepAngularVelocityThreshold=0;Wood->SleepCounterThreshold=100000;}
    Body->SetPhysMaterialOverride(Wood);
    if(CanBreak){
        FallHeading=Direction.GetSafeNormal2D();if(FallHeading.IsNearlyZero())FallHeading=FVector::ForwardVector;
        const FVector Axis=FVector::CrossProduct(FVector::UpVector,FallHeading).GetSafeNormal();
        const FVector Pivot=Source.TransformPosition(FVector(Trunk.GetCenter().X,Trunk.GetCenter().Y,Trunk.Min.Z));
        // A 1.5 degree lean breaks the perfectly upright equilibrium. Start at
        // rest: gravity, not a scripted angular velocity, accelerates the fall.
        Body->SetPhysicsLinearVelocity(FVector::ZeroVector);Body->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
        StumpHinge=NewObject<UPhysicsConstraintComponent>(this);StumpHinge->RegisterComponent();
        StumpHinge->SetWorldLocation(Pivot);StumpHinge->SetWorldRotation(Axis.Rotation());
        StumpHinge->SetLinearXLimit(LCM_Locked,0);StumpHinge->SetLinearYLimit(LCM_Locked,0);StumpHinge->SetLinearZLimit(LCM_Locked,0);
        StumpHinge->SetAngularSwing1Limit(ACM_Locked,0);StumpHinge->SetAngularSwing2Limit(ACM_Locked,0);StumpHinge->SetAngularTwistLimit(ACM_Free,0);
        StumpHinge->SetConstrainedComponents(Body,NAME_None,nullptr,NAME_None);
        Body->WakeAllRigidBodies();
        UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling HINGE heading=%s initialLeanDeg=1.5 initialAngularSpeed=0 up=%s position=%s"),*FallHeading.ToCompactString(),*GetActorUpVector().ToCompactString(),*GetActorLocation().ToCompactString());
    }
    UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling BODY %s massKg=%.1f visualSections=%d breakable=%d"),*GetName(),Body->GetMass(),Visuals.Num(),int(Breakable));
    if(!DeferringGeometrySnapshot)RefreshGeometrySnapshot();
}
void AVoxelFallingTimber::Contact(UPrimitiveComponent*,AActor* Other,UPrimitiveComponent*,FVector Impulse,const FHitResult& Hit){
    if(!Other||!Other->ActorHasTag(TEXT("TreeFellingGround")))return;
    const FVector N=Hit.ImpactNormal.GetSafeNormal();
    const FVector PointVelocity=LastLinear+FVector::CrossProduct(LastAngular,Hit.ImpactPoint-LastCenter);
    const double ClosingSpeed=FMath::Max(0.,-FVector::DotProduct(PointVelocity,N))*.01;
    const double Mass=Body->GetMass();
    const FVector Size=Trunk.GetSize()*.01;
    const FVector Inertia=Mass/12.*FVector(Size.Y*Size.Y+Size.Z*Size.Z,Size.X*Size.X+Size.Z*Size.Z,Size.X*Size.X+Size.Y*Size.Y);
    const FVector R=GetActorQuat().UnrotateVector(Hit.ImpactPoint-LastCenter)*.01;
    const FVector LocalN=GetActorQuat().UnrotateVector(N),Moment=FVector::CrossProduct(R,LocalN);
    const double InvMass=1./Mass+Moment.X*Moment.X/Inertia.X+Moment.Y*Moment.Y/Inertia.Y+Moment.Z*Moment.Z/Inertia.Z;
    const double EffectiveMass=1./InvMass,Energy=.5*EffectiveMass*ClosingSpeed*ClosingSpeed;
    // Keep a peak per contact episode; repeated solver callbacks must not
    // manufacture additional damage by counting the same impact repeatedly.
    if(Energy>PeakImpactJ){
        PeakImpactJ=Energy;
        UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling IMPACT %s energyJ=%.1f closingMps=%.2f effectiveMassKg=%.1f impulseNs=%.1f"),*GetName(),Energy,ClosingSpeed,EffectiveMass,Impulse.Size()*.01);
    }
    if(StumpHinge&&Age>.5&&FVector::Dist(Hit.ImpactPoint,StumpHinge->GetComponentLocation())>100)ReleaseHinge=true;
    // Provisional gameplay fracture energy per cross-sectional area. This is
    // an instrumented threshold, not a calibrated green-wood strength model.
    const double FractureJ=Size.X*Size.Y*6000.;
    if(Breakable&&ReleaseHinge&&Energy>FractureJ)PendingBreak=true;
}
FBox AVoxelFallingTimber::GetGroundSupportBounds() const {
    if(!Trunk.IsValid)return FBox(GetActorLocation()-FVector(1400),GetActorLocation()+FVector(1400));
    // The whole trunk can rotate around its stump, or around its mass center
    // after release. Include a two-second linear lookahead and 4 m guard band.
    const bool Hinged=StumpHinge||(PreparedRestore&&PreparedRestore->Hinged);
    const FVector Center=StumpHinge?StumpHinge->GetComponentLocation():PreparedRestore&&PreparedRestore->Hinged?PreparedRestore->HingeTransform.GetLocation():GetActorLocation();
    const double Radius=(Hinged?Trunk.GetSize().Size():Trunk.GetExtent().Size())+400.;
    FBox Bounds(Center-FVector(Radius),Center+FVector(Radius));
    const FVector Velocity=PreparedRestore?PreparedRestore->Linear:GroundPaused?GroundLinear:Body->GetPhysicsLinearVelocity();
    const FBox Future(Bounds.Min+Velocity*2.,Bounds.Max+Velocity*2.);Bounds+=Future;
    return Bounds;
}
void AVoxelFallingTimber::PauseForGroundSupport(){
    if(GroundPaused||!Body||StagedRestore||PreparedRestore||ActorHasTag(TEXT("ObjectRestorePending"))||GetNetMode()==NM_Client)return;
    GroundLinear=Body->GetPhysicsLinearVelocity();GroundAngular=Body->GetPhysicsAngularVelocityInRadians();
    GroundWasAwake=Body->IsAnyRigidBodyAwake();GroundPaused=true;Body->SetSimulatePhysics(false);
}
void AVoxelFallingTimber::Tick(float Dt){
    Super::Tick(Dt);
    if(GetNetMode()!=NM_Client){
        if(!VoxelTreeFelling::AdvanceRestoreGround(GetWorld(),GetGroundSupportBounds())){
            PauseForGroundSupport();
            return;
        }
        if(GroundPaused){
            Body->SetSimulatePhysics(true);
            if(StumpHinge)StumpHinge->SetConstrainedComponents(Body,NAME_None,nullptr,NAME_None);
            Body->SetPhysicsLinearVelocity(GroundLinear);Body->SetPhysicsAngularVelocityInRadians(GroundAngular);if(!GroundWasAwake&&!StumpHinge)Body->PutAllRigidBodiesToSleep();GroundPaused=false;
        }
    }
    Age+=Dt;
    LastLinear=Body->GetPhysicsLinearVelocity();LastAngular=Body->GetPhysicsAngularVelocityInRadians();LastCenter=Body->GetCenterOfMass();
    if(ReleaseHinge&&StumpHinge){
        StumpHinge->BreakConstraint();StumpHinge->DestroyComponent();StumpHinge=nullptr;
        auto RestMaterial=NewObject<UPhysicalMaterial>(this);RestMaterial->Friction=.8f;RestMaterial->Restitution=0;Body->SetPhysMaterialOverride(RestMaterial);
        UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling HINGE_RELEASE groundContact=1"));
    }
    PreviousSpeed=Body->GetPhysicsLinearVelocity().Size()+Body->GetPhysicsAngularVelocityInRadians().Size()*Trunk.GetExtent().Z;
    if(PendingBreak){PendingBreak=false;BreakOnImpact();return;}
    if(int(Age)!=int(Age-Dt)&&Age<35)UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling MOTION %s age=%.1f speed=%.1f angular=%.1f position=%s"),*GetName(),Age,Body->GetPhysicsLinearVelocity().Size(),Body->GetPhysicsAngularVelocityInDegrees().Size(),*GetActorLocation().ToCompactString());
    if(!StumpHinge&&!ReportedRest&&Age>3&&PreviousSpeed<10){ReportedRest=true;UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling REST %s age=%.2f tilt=%.1f"),*GetName(),Age,FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(GetActorUpVector().Z,-1.,1.))));}
    // Substantial fragments remain in the world. Chaos can wake them on later
    // contact without needing the prototype's per-frame diagnostic tick.
    // Keep the inexpensive support admission tick: sleeping bodies may wake later.
}
void AVoxelFallingTimber::BreakOnImpact(){
    Breakable=false;
    if(Visuals.Num()<2)return;
    // Split on a render-section boundary. This deliberately limits the first
    // impact prototype to two substantial timber pieces, not voxel confetti.
    const double Split=FMath::RoundToDouble((Trunk.Min.Z+Trunk.Max.Z)*.5/160.)*160.;
    TArray<UProceduralMeshComponent*> Groups[2];
    for(auto C:Visuals){const auto S=C->GetProcMeshSection(0);
        const int Group=C->ComponentHasTag(TEXT("FractureLowerCap"))?0:C->ComponentHasTag(TEXT("FractureUpperCap"))?1:S&&S->SectionLocalBox.GetCenter().Z>=Split?1:0;
        Groups[Group].Add(C);
    }
    if(Groups[0].IsEmpty()||Groups[1].IsEmpty())return;
    const FTransform Source=Visuals[0]->GetComponentTransform();
    const FVector Angular=Body->GetPhysicsAngularVelocityInRadians();
    const FVector Linear=Body->GetPhysicsLinearVelocity(),Center=Body->GetCenterOfMass();
    Body->SetSimulatePhysics(false);Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    for(int I=0;I<2;++I){
        FBox Part=Trunk;if(I)Part.Min.Z=Split;else Part.Max.Z=Split;
        auto Fragment=GetWorld()->SpawnActor<AVoxelFallingTimber>();
        const FVector V=Linear+FVector::CrossProduct(Angular,Source.TransformPosition(Part.GetCenter())-Center);
        Fragment->Initialize(Groups[I],Part,Source,FVector::ZeroVector,false);
        Fragment->Body->SetPhysicsLinearVelocity(V);Fragment->Body->SetPhysicsAngularVelocityInRadians(Angular);
    }
    Visuals.Reset();GeometrySnapshot.Reset();UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling BREAK %s fragments=2 splitLocalZ=%.1f"),*GetName(),Split);Destroy();
}
bool AVoxelFallingTimber::PersistentState(FArchive& Ar){
    if(StagedRestore||PreparedRestore){if(Ar.IsLoading())CancelStagedObjectRestore();return false;}
    return SerializePersistentState(Ar,false);
}
bool AVoxelFallingTimber::SerializePersistentState(FArchive& Ar,bool GeometryOnly){
    using namespace VoxelDetachedPersistence;
    FTransform Transform=GetActorTransform(),HingeTransform=StumpHinge?StumpHinge->GetComponentTransform():FTransform::Identity;
    FVector Linear=GroundPaused?GroundLinear:Body->GetPhysicsLinearVelocity(),Angular=GroundPaused?GroundAngular:Body->GetPhysicsAngularVelocityInRadians();
    bool Hinged=StumpHinge!=nullptr,Awake=GroundPaused?GroundWasAwake:Body->IsAnyRigidBodyAwake();
    auto Lifetime=Cleanup->CaptureState();uint8 Kind=uint8(Lifetime.Kind);
    if(!GeometryOnly)Ar<<Trunk<<Transform<<Linear<<Angular<<Hinged<<HingeTransform<<Awake<<Breakable<<FallHeading<<Age<<Kind<<Lifetime.RemainingSeconds;
    if(Ar.IsError()||!Trunk.IsValid||Trunk.GetSize().GetMin()<=0||Trunk.GetSize().GetMax()>100000||Transform.ContainsNaN()||Linear.ContainsNaN()||Angular.ContainsNaN()||HingeTransform.ContainsNaN()||!FMath::IsFinite(Age)||FallHeading.ContainsNaN()||Kind>3)return false;
    int32 Count=Visuals.Num();Ar<<Count;if(Ar.IsError()||Count<1||Count>4096)return false;
    if(Ar.IsLoading())SetActorTransform(Transform);
    TArray<UProceduralMeshComponent*> Loaded;TArray<bool> Visibility;
    for(int32 I=0;I<Count;++I){
        FProcMeshSection Section;const FProcMeshSection* SourceSection=nullptr;FTransform Relative;bool Visible=true;uint8 Cap=0;
        if(Ar.IsSaving()){
            auto C=Visuals[I];auto S=C->GetProcMeshSection(0);if(!S)return false;
            SourceSection=S;Relative=C->GetComponentTransform().GetRelativeTransform(Transform);Visible=C->IsVisible();
            Cap=C->ComponentHasTag(TEXT("FractureLowerCap"))?1:C->ComponentHasTag(TEXT("FractureUpperCap"))?2:0;
            Cap=AppearanceFlags(C,Cap);
        }
        Ar<<Relative<<Visible<<Cap;
        if(Ar.CustomVer(VoxelPackedTimberMesh::VersionKey)>=1){
            if(!VoxelPackedTimberMesh::Serialize(Ar,Section,SourceSection))return false;
        }else{
            if(Ar.IsSaving())Section=*SourceSection;
            if(!Array(Ar,Section.ProcVertexBuffer,262144,[](FArchive& A,FProcMeshVertex& V){A<<V.Position<<V.Normal<<V.UV0<<V.UV1<<V.Color<<V.Tangent.TangentX<<V.Tangent.bFlipTangentY;}))return false;
            if(!Array(Ar,Section.ProcIndexBuffer,393216,[](FArchive& A,uint32& Index){A<<Index;}))return false;
        }
        if(Ar.IsError()||Relative.ContainsNaN()||!ValidAppearanceFlags(Cap)||Section.ProcIndexBuffer.Num()%3)return false;
        if(Ar.IsLoading()){
            Section.SectionLocalBox=FBox(ForceInit);
            for(auto& V:Section.ProcVertexBuffer){if(V.Position.ContainsNaN()||V.Normal.ContainsNaN()||V.UV0.ContainsNaN()||V.UV1.ContainsNaN()||V.Tangent.TangentX.ContainsNaN())return false;Section.SectionLocalBox+=V.Position;}
            for(auto Index:Section.ProcIndexBuffer)if(Index>=uint32(Section.ProcVertexBuffer.Num()))return false;
            auto C=NewObject<UProceduralMeshComponent>(this);C->SetupAttachment(Body);C->RegisterComponent();
            C->SetWorldTransform(Relative*Transform);C->SetProcMeshSection(0,Section);C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            auto Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelEnvironmentLOD.M_VoxelEnvironmentLOD"));if(!Base)return false;
            auto Mat=UMaterialInstanceDynamic::Create(Base,this);Mat->SetScalarParameterValue(TEXT("WindEnabled"),0);Mat->SetScalarParameterValue(TEXT("Fade"),1);Mat->SetScalarParameterValue(TEXT("Reverse"),0);C->SetMaterial(0,Mat);
            RestoreAppearanceFlags(Mat,Cap);
            if(Cap&3)C->ComponentTags.Add((Cap&3)==1?TEXT("FractureLowerCap"):TEXT("FractureUpperCap"));
            Loaded.Add(C);Visibility.Add(Visible);
        }
    }
    if(Ar.IsLoading()){
        const bool SavedBreakable=Breakable;
        const FTransform Source(Transform.GetRotation(),Transform.GetLocation()-Transform.GetRotation().RotateVector(Trunk.GetCenter()));
        DeferringGeometrySnapshot=true;
        Initialize(Loaded,Trunk,Source,FVector::ZeroVector,false);Breakable=SavedBreakable;
        DeferringGeometrySnapshot=false;
        for(int32 I=0;I<Loaded.Num();++I)Loaded[I]->SetVisibility(Visibility[I]);
        if(Hinged){
            StumpHinge=NewObject<UPhysicsConstraintComponent>(this);StumpHinge->RegisterComponent();StumpHinge->SetWorldTransform(HingeTransform);
            StumpHinge->SetLinearXLimit(LCM_Locked,0);StumpHinge->SetLinearYLimit(LCM_Locked,0);StumpHinge->SetLinearZLimit(LCM_Locked,0);
            StumpHinge->SetAngularSwing1Limit(ACM_Locked,0);StumpHinge->SetAngularSwing2Limit(ACM_Locked,0);StumpHinge->SetAngularTwistLimit(ACM_Free,0);
            StumpHinge->SetConstrainedComponents(Body,NAME_None,nullptr,NAME_None);
            auto Wood=NewObject<UPhysicalMaterial>(this);Wood->Friction=.8f;Wood->Restitution=0;Wood->SleepLinearVelocityThreshold=0;Wood->SleepAngularVelocityThreshold=0;Wood->SleepCounterThreshold=100000;Body->SetPhysMaterialOverride(Wood);
        }
        VoxelTreeFelling::PrepareGround(GetWorld(),Transform.GetLocation());
        Body->SetPhysicsLinearVelocity(Linear);Body->SetPhysicsAngularVelocityInRadians(Angular);
        if(!Awake&&!Hinged)Body->PutAllRigidBodiesToSleep();
        Lifetime.Kind=EVoxelDebrisLifetime(Kind);Cleanup->RestoreState(Lifetime);
        RefreshGeometrySnapshot();
    }
    return !Ar.IsError();
}
bool AVoxelFallingTimber::RefreshGeometrySnapshot(){
    check(IsInGameThread());GeometrySnapshot.Reset();
    if(Visuals.IsEmpty())return false;
    const double Started=FPlatformTime::Seconds();
    auto Bytes=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();FMemoryWriter Writer(*Bytes);
    VoxelObjectGeometrySnapshot::WriteVersion(Writer);
    Writer.SetCustomVersion(VoxelPackedTimberMesh::VersionKey,1,NAME_None);
    if(!SerializePersistentState(Writer,true)||Writer.IsError())return false;
    GeometrySnapshot=Bytes;
    UE_LOG(LogVoxelEarth,Verbose,TEXT("ObjectGeometry CACHE timber %s sections=%d bytes=%d ms=%.3f"),*GetName(),Visuals.Num(),Bytes->Num(),(FPlatformTime::Seconds()-Started)*1000.);
    return true;
}
bool AVoxelFallingTimber::CaptureObjectState(FVoxelImmutableGeometry& Geometry,TArray<uint8>& Dynamic){
    check(IsInGameThread());Geometry.Reset();Dynamic.Reset();if(StagedRestore||PreparedRestore||!GeometrySnapshot)return false;
    FTransform Transform=GetActorTransform(),HingeTransform=StumpHinge?StumpHinge->GetComponentTransform():FTransform::Identity;
    FVector Linear=GroundPaused?GroundLinear:Body->GetPhysicsLinearVelocity(),Angular=GroundPaused?GroundAngular:Body->GetPhysicsAngularVelocityInRadians();
    bool Hinged=StumpHinge!=nullptr,Awake=GroundPaused?GroundWasAwake:Body->IsAnyRigidBodyAwake();
    auto Lifetime=Cleanup->CaptureState();uint8 Kind=uint8(Lifetime.Kind);
    if(Transform.ContainsNaN()||Linear.ContainsNaN()||Angular.ContainsNaN()||HingeTransform.ContainsNaN()||!FMath::IsFinite(Age))return false;
    FMemoryWriter Writer(Dynamic);VoxelObjectGeometrySnapshot::WriteVersion(Writer);
    // Small legacy header only. Relative section transforms and mesh buffers
    // are invariant during rigid motion and reside in GeometrySnapshot.
    Writer<<Trunk<<Transform<<Linear<<Angular<<Hinged<<HingeTransform<<Awake<<Breakable<<FallHeading<<Age<<Kind<<Lifetime.RemainingSeconds;
    if(Writer.IsError())return false;Geometry=GeometrySnapshot;return true;
}
bool AVoxelFallingTimber::RestoreObjectState(const TArray<uint8>& Geometry,const TArray<uint8>& Dynamic){
    check(IsInGameThread());TArray<uint8> Legacy;
    if(!VoxelObjectGeometrySnapshot::Combine(Geometry,Dynamic,Legacy))return false;
    FMemoryReader Reader(Legacy);Reader.SetCustomVersion(VoxelPackedTimberMesh::VersionKey,1,NAME_None);
    return PersistentState(Reader)&&!Reader.IsError()&&Reader.Tell()==Reader.TotalSize();
}
void AVoxelFallingTimber::CancelStagedObjectRestore(){
    check(IsInGameThread());auto Job=StagedRestore?MoveTemp(StagedRestore):MoveTemp(PreparedRestore);if(!Job)return;
    Job->Cancelled.Store(true);Body->SetSimulatePhysics(false);Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    for(auto C:Visuals)if(IsValid(C))C->DestroyComponent();Visuals.Reset();GeometrySnapshot.Reset();
    auto Done=MoveTemp(Job->Completion);if(Done)Done(false);
}
void AVoxelFallingTimber::BeginStagedObjectRestore(FVoxelImmutableGeometry Geometry,TArray<uint8> Dynamic,TFunction<void(bool)> Completion){
    check(IsInGameThread());CancelStagedObjectRestore();
    if(!Visuals.IsEmpty()||!Geometry||Geometry->Num()<4||Dynamic.Num()<4||Dynamic.Num()>4096||int64(Geometry->Num())+Dynamic.Num()>VoxelObjectGeometrySnapshot::MaxBytes){if(Completion)Completion(false);return;}
    SetActorHiddenInGame(true);SetActorEnableCollision(false);SetActorTickEnabled(false);Body->SetSimulatePhysics(false);Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    auto Job=MakeShared<FTimberStagedRestore,ESPMode::ThreadSafe>();Job->Geometry=MoveTemp(Geometry);Job->Completion=MoveTemp(Completion);StagedRestore=Job;
    TWeakObjectPtr<AVoxelFallingTimber> Weak(this);
    Async(EAsyncExecution::ThreadPool,[Job,Weak,Dynamic=MoveTemp(Dynamic)](){
        auto Decode=[&](){
            FMemoryReader D(Dynamic),G(*Job->Geometry);uint32 DV=0,GV=0;D<<DV;G<<GV;if(DV!=1||GV!=1)return false;
            uint8 Kind=0;D<<Job->Trunk<<Job->Transform<<Job->Linear<<Job->Angular<<Job->Hinged<<Job->HingeTransform<<Job->Awake<<Job->Breakable<<Job->Heading<<Job->Age<<Kind<<Job->Lifetime.RemainingSeconds;
            if(D.IsError()||D.Tell()!=D.TotalSize()||!Job->Trunk.IsValid||Job->Trunk.Min.ContainsNaN()||Job->Trunk.Max.ContainsNaN()||Job->Trunk.GetSize().GetMin()<=0||Job->Trunk.GetSize().GetMax()>100000||Job->Transform.ContainsNaN()||Job->Linear.ContainsNaN()||Job->Angular.ContainsNaN()||Job->HingeTransform.ContainsNaN()||Job->Heading.ContainsNaN()||!FMath::IsFinite(Job->Age)||!FMath::IsFinite(Job->Lifetime.RemainingSeconds)||Kind>3)return false;
            Job->Lifetime.Kind=EVoxelDebrisLifetime(Kind);
            int32 Count=0;G<<Count;if(G.IsError()||Count<1||Count>4096)return false;
            Job->Sections.Reserve(Count);
            for(int I=0;I<Count;++I){
                if(Job->Cancelled.Load())return false;
                FTimberStagedRestore::FSection Section;G<<Section.Relative<<Section.Visible<<Section.Cap;
                if(G.IsError()||Section.Relative.ContainsNaN()||!ValidAppearanceFlags(Section.Cap)||!VoxelPackedTimberMesh::Serialize(G,Section.Mesh,nullptr))return false;
                Section.Mesh.SectionLocalBox=FBox(ForceInit);
                for(const auto& V:Section.Mesh.ProcVertexBuffer){if(V.Position.ContainsNaN()||V.Normal.ContainsNaN()||V.UV0.ContainsNaN()||V.UV1.ContainsNaN()||V.Tangent.TangentX.ContainsNaN())return false;Section.Mesh.SectionLocalBox+=V.Position;}
                for(auto Index:Section.Mesh.ProcIndexBuffer)if(Index>=uint32(Section.Mesh.ProcVertexBuffer.Num()))return false;
                Job->Sections.Add(MoveTemp(Section));
            }
            return !G.IsError()&&G.Tell()==G.TotalSize()&&!Job->Cancelled.Load();
        };
        const bool Valid=Decode();
        AsyncTask(ENamedThreads::GameThread,[Job,Weak,Valid](){if(auto A=Weak.Get())if(A->StagedRestore==Job&&!Job->Cancelled.Load()){Job->Valid=Valid;Job->Ready=true;}});
    });
}
bool AVoxelFallingTimber::AdvanceStagedObjectRestore(){
    check(IsInGameThread());auto Job=StagedRestore;if(!Job||!Job->Ready)return false;
    if(!Job->Valid){CancelStagedObjectRestore();return true;}
    if(!Job->Adopted){
        Trunk=Job->Trunk;SetActorTransform(Job->Transform);Body->SetBoxExtent(Trunk.GetExtent().ComponentMax(FVector(5)));
        Breakable=Job->Breakable;FallHeading=Job->Heading;Age=Job->Age;
        Job->Adopted=true;return true;
    }
    if(Job->Next<Job->Sections.Num()){
        auto Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelEnvironmentLOD.M_VoxelEnvironmentLOD"));if(!Base){CancelStagedObjectRestore();return true;}
        auto& S=Job->Sections[Job->Next++];auto C=NewObject<UProceduralMeshComponent>(this);C->SetupAttachment(Body);C->SetRelativeTransform(S.Relative);C->SetCollisionEnabled(ECollisionEnabled::NoCollision);C->SetVisibility(S.Visible);C->RegisterComponent();C->SetProcMeshSection(0,S.Mesh);
        auto Mat=UMaterialInstanceDynamic::Create(Base,this);Mat->SetScalarParameterValue(TEXT("WindEnabled"),0);Mat->SetScalarParameterValue(TEXT("Fade"),1);Mat->SetScalarParameterValue(TEXT("Reverse"),0);C->SetMaterial(0,Mat);
        RestoreAppearanceFlags(Mat,S.Cap);
        if(S.Cap&3)C->ComponentTags.Add((S.Cap&3)==1?TEXT("FractureLowerCap"):TEXT("FractureUpperCap"));
        Visuals.Add(C);S.Mesh=FProcMeshSection();return true;
    }
    GeometrySnapshot=Job->Geometry;PreparedRestore=Job;StagedRestore.Reset();
    auto Done=MoveTemp(Job->Completion);if(Done)Done(true);return true;
}
bool AVoxelFallingTimber::PublishStagedObjectRestore(){
    check(IsInGameThread());auto Job=MoveTemp(PreparedRestore);if(!Job)return false;
    Cleanup->RestoreState(Job->Lifetime);SetActorHiddenInGame(false);
    if(GetNetMode()==NM_Client){SetActorTickEnabled(false);return true;}
    SetActorEnableCollision(true);Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);Body->SetSimulatePhysics(true);
    Body->SetMassOverrideInKg(NAME_None,float(FMath::Max(5.,Trunk.GetVolume()/1000000.*550.)),true);Body->SetMaxDepenetrationVelocity(NAME_None,200.f);Body->SetPhysicsMaxAngularVelocityInDegrees(90.f);
    Body->BodyInstance.SetPositionSolverIterationCount(16);Body->BodyInstance.SetVelocitySolverIterationCount(8);
    auto Wood=NewObject<UPhysicalMaterial>(this);Wood->Friction=.8f;Wood->Restitution=0;
    if(Job->Hinged){
        Wood->SleepLinearVelocityThreshold=0;Wood->SleepAngularVelocityThreshold=0;Wood->SleepCounterThreshold=100000;
        StumpHinge=NewObject<UPhysicsConstraintComponent>(this);StumpHinge->RegisterComponent();StumpHinge->SetWorldTransform(Job->HingeTransform);
        StumpHinge->SetLinearXLimit(LCM_Locked,0);StumpHinge->SetLinearYLimit(LCM_Locked,0);StumpHinge->SetLinearZLimit(LCM_Locked,0);
        StumpHinge->SetAngularSwing1Limit(ACM_Locked,0);StumpHinge->SetAngularSwing2Limit(ACM_Locked,0);StumpHinge->SetAngularTwistLimit(ACM_Free,0);StumpHinge->SetConstrainedComponents(Body,NAME_None,nullptr,NAME_None);
    }
    Body->SetPhysMaterialOverride(Wood);Body->SetPhysicsLinearVelocity(Job->Linear);Body->SetPhysicsAngularVelocityInRadians(Job->Angular);
    if(!Job->Awake&&!Job->Hinged)Body->PutAllRigidBodiesToSleep();SetActorTickEnabled(true);return true;
}
void AVoxelFallingTimber::EndPlay(const EEndPlayReason::Type Reason){
    CancelStagedObjectRestore();
    VoxelDetachedPersistence::OnEndPlay(this,Reason);
    for(auto C:Visuals)if(IsValid(C))C->DestroyComponent();Visuals.Reset();Super::EndPlay(Reason);
}

AVoxelStoneAxePrototype::AVoxelStoneAxePrototype(){
    PrimaryActorTick.bCanEverTick=true;
    Mesh=CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("StoneAxe"));SetRootComponent(Mesh);
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);Mesh->SetCastShadow(false);
}
bool AVoxelStoneAxePrototype::Swing(){if(!Equipped)return false;if(SwingAge>=.65){SwingAge=0;Struck=false;}return true;}
void AVoxelStoneAxePrototype::Tick(float Dt){
    Super::Tick(Dt);auto PC=GetWorld()->GetFirstPlayerController();if(!PC)return;
    Mesh->SetVisibility(Equipped&&!PC->bShowMouseCursor);
    FVector Eye;FRotator Aim;PC->GetPlayerViewPoint(Eye,Aim);
    SwingAge+=Dt;
    const double SwingAngle=SwingAge<.65?FMath::Sin(SwingAge/.65*PI)*75.:0.;
    SetActorLocation(Eye+Aim.RotateVector(FVector(22,27,-28)));
    SetActorRotation((Aim.Quaternion()*FRotator(30-SwingAngle,-8,15).Quaternion()).Rotator());
    if(Equipped&&!Struck&&SwingAge>=.22&&SwingAge<.65){Struck=true;
        const auto VoxelPC=Cast<AVoxelEarthPlayerController>(PC);
        const bool Hit=VoxelPC&&VoxelPC->RequestChop(Eye,Aim.Vector(),VoxelPC->GetDigSizeVoxels());
        UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling AXE_STRIKE hit=%d"),int(Hit));
    }
}
namespace VoxelTreeFelling {
namespace {
struct FGroundRestore {
    TWeakObjectPtr<AActor> Actor;
    FVector Center;
    TArray<FVector> Vertices;
    int32 Tile=0,NextTile=0;
    TSet<int32> DirtyTiles;
    TArray<TWeakObjectPtr<UProceduralMeshComponent>> Sections;
    bool Ready=false;
    int32 TilesX=10,TilesY=10;
};
TMap<TWeakObjectPtr<UWorld>,TArray<TSharedPtr<FGroundRestore>>> GroundRestores;
TMap<TWeakObjectPtr<UWorld>,uint64> GroundFrames;
struct FGroundRestoreLifetime {
    FDelegateHandle Handle;
    FGroundRestoreLifetime(){Handle=FWorldDelegates::OnWorldBeginTearDown.AddLambda([](UWorld* W){GroundRestores.Remove(W);GroundFrames.Remove(W);});}
    ~FGroundRestoreLifetime(){FWorldDelegates::OnWorldBeginTearDown.Remove(Handle);}
} GroundRestoreLifetime;
bool CanReclaimGround(UWorld* W,const FGroundRestore& Ground,const FVector& Requested){
    const auto Near=[&](const FVector& P,double Radius){return FMath::Square(P.X-Ground.Center.X)+FMath::Square(P.Y-Ground.Center.Y)<=Radius*Radius;};
    if(Near(Requested,6400.))return false;
    for(auto It=W->GetPlayerControllerIterator();It;++It)if(auto PC=It->Get()){
        FVector Eye;FRotator Rotation;PC->GetPlayerViewPoint(Eye,Rotation);if(Near(Eye,10000.))return false;
    }
    for(TActorIterator<AVoxelEnvironmentLODPrototype> It(W);It;++It)
        if((It->IsFellable()||It->ActorHasTag(TEXT("ObjectRestorePending")))&&Near(It->GetActorLocation(),3500.))return false;
    const FBox Proxy(Ground.Center-FVector(Ground.TilesX*160.,Ground.TilesY*160.,0),Ground.Center+FVector(Ground.TilesX*160.,Ground.TilesY*160.,0));
    for(TActorIterator<AVoxelFallingTimber> It(W);It;++It){
        FBox B=It->GetGroundSupportBounds();
        if(It->ActorHasTag(TEXT("ObjectRestorePending"))){
            // The bridge assigns the entry transform before worker dispatch.
            // A placeholder box does not yet include the future whole trunk.
            const FVector P=It->GetActorLocation();B+=P-FVector(1400);B+=P+FVector(1400);
        }
        if(B.IsValid&&B.Min.X<=Proxy.Max.X&&B.Max.X>=Proxy.Min.X&&B.Min.Y<=Proxy.Max.Y&&B.Max.Y>=Proxy.Min.Y)return false;
    }
    return true;
}
}
void EnsureAxe(UWorld* W){
    if(!W)return;for(TActorIterator<AVoxelStoneAxePrototype> It(W);It;++It)return;
    auto Axe=W->SpawnActor<AVoxelStoneAxePrototype>();if(Axe&&!Axe->Initialize())Axe->Destroy();
}
bool AdvanceRestoreGround(UWorld* W,const FVector& Location){return AdvanceRestoreGround(W,FBox(Location-FVector(200),Location+FVector(200)));}
bool AdvanceRestoreGround(UWorld* W,const FBox& Bounds){
    if(!Bounds.IsValid||Bounds.Min.ContainsNaN()||Bounds.Max.ContainsNaN()||Bounds.GetSize().GetMax()>1000000.)return false;const FVector Location=Bounds.GetCenter();
    const int32 TilesX=FMath::Max(1,FMath::CeilToInt(Bounds.GetSize().X/320.)+1),TilesY=FMath::Max(1,FMath::CeilToInt(Bounds.GetSize().Y/320.)+1);
    if(int64(TilesX)*TilesY>16384)return false;
    check(IsInGameThread());if(!W||W->bIsTearingDown||Location.ContainsNaN())return false;
    if(W->GetNetMode()==NM_Client)return true;
    auto& Jobs=GroundRestores.FindOrAdd(W);TSharedPtr<FGroundRestore> Job;
    // Reuse only when the complete requested footprint is covered.
    for(auto Candidate:Jobs)if(Candidate->Actor.IsValid()&&Bounds.Min.X>=Candidate->Center.X-Candidate->TilesX*160.&&Bounds.Max.X<=Candidate->Center.X+Candidate->TilesX*160.&&Bounds.Min.Y>=Candidate->Center.Y-Candidate->TilesY*160.&&Bounds.Max.Y<=Candidate->Center.Y+Candidate->TilesY*160.){Job=Candidate;break;}
    if(!Job){
        for(TActorIterator<AActor> It(W);It;++It)if(It->ActorHasTag(TEXT("TreeFellingGround"))&&!It->ActorHasTag(TEXT("GroundRestorePartial"))){
            const FBox B=It->GetComponentsBoundingBox(true);
            if(B.IsValid&&B.Min.X<=Bounds.Min.X&&B.Max.X>=Bounds.Max.X&&B.Min.Y<=Bounds.Min.Y&&B.Max.Y>=Bounds.Max.Y)return true;
        }
        Jobs.RemoveAll([](const TSharedPtr<FGroundRestore>& Existing){return !Existing->Actor.IsValid();});
        int64 ReservedTiles=int64(TilesX)*TilesY;for(const auto& Existing:Jobs)ReservedTiles+=int64(Existing->TilesX)*Existing->TilesY;
        if(Jobs.Num()>=32||ReservedTiles>16384){
            int32 Reclaim=INDEX_NONE;double Furthest=-1;
            for(int I=0;I<Jobs.Num();++I)if(CanReclaimGround(W,*Jobs[I],Location)){
                const double Distance=FMath::Square(Jobs[I]->Center.X-Location.X)+FMath::Square(Jobs[I]->Center.Y-Location.Y);
                if(Distance>Furthest){Furthest=Distance;Reclaim=I;}
            }
            // At most one proxy teardown in this call. Admission resumes on
            // the next step. If the footprint/section budget is occupied, defer safely.
            if(Reclaim!=INDEX_NONE){auto Old=Jobs[Reclaim]->Actor.Get();Jobs.RemoveAtSwap(Reclaim);if(Old)Old->Destroy();}
            return false;
        }
        if(!W->GetSubsystem<UVoxelWorldSubsystem>())return false;
        auto Settings=UPhysicsSettings::Get();Settings->bSubstepping=true;Settings->MaxSubstepDeltaTime=1.f/120.f;Settings->MaxSubsteps=16;
        Job=MakeShared<FGroundRestore>();Job->TilesX=TilesX;Job->TilesY=TilesY;Job->Sections.SetNum(TilesX*TilesY);Job->Center=FVector(FMath::GridSnap(Location.X,10.),FMath::GridSnap(Location.Y,10.),Location.Z);
        auto A=W->SpawnActor<AActor>();if(!A)return false;Job->Actor=A;A->Tags.Add(TEXT("TreeFellingGround"));A->Tags.Add(TEXT("GroundRestorePartial"));
        auto Root=NewObject<USceneComponent>(A);A->SetRootComponent(Root);A->SetActorLocation(Job->Center);Root->SetMobility(EComponentMobility::Static);Root->RegisterComponent();Jobs.Add(Job);
    }
    if(Job->Ready)return true;
    auto A=Job->Actor.Get();auto Sub=W->GetSubsystem<UVoxelWorldSubsystem>();if(!A||!Sub)return false;
    // Global per-world frame gate includes publication jobs and moving bodies.
    constexpr int32 TileCells=16,Side=17;constexpr double Step=20.;
    const int32 TileCount=Job->TilesX*Job->TilesY;
    if(Job->Vertices.IsEmpty())Job->Tile=Job->DirtyTiles.Num()?*Job->DirtyTiles.CreateConstIterator():Job->NextTile;
    if(Job->Tile>=TileCount){Job->Ready=true;return true;}
    const double TileMinX=Job->Center.X-Job->TilesX*160.+(Job->Tile/Job->TilesY)*320.;
    const double TileMinY=Job->Center.Y-Job->TilesY*160.+(Job->Tile%Job->TilesY)*320.;
    auto Streamer=Sub->GetFineTileStreamer();
    if(!Streamer||!Streamer->IsFootprintResident(FMath::FloorToInt64(TileMinX*10),FMath::FloorToInt64(TileMinY*10),
        FMath::CeilToInt64((TileMinX+320)*10)+1,FMath::CeilToInt64((TileMinY+320)*10)+1))return false;
    if(const auto F=GroundFrames.Find(W);F&&*F==GFrameCounter)return false;
    GroundFrames.Add(W,GFrameCounter);
    const double Start=FPlatformTime::Seconds();int32 Samples=0;
    // Residency was checked above; no fine-tier loading is initiated here.
    while(Job->Vertices.Num()<Side*Side&&Samples<64&&FPlatformTime::Seconds()-Start<.001){
        const int32 I=Job->Vertices.Num(),X=I/Side,Y=I%Side;
        const double WX=TileMinX+X*Step,WY=TileMinY+Y*Step;
        double Z=Sub->GetSurfaceHeightUU(WX,WY);FVector Hit,Previous;
        if(Sub->RaycastVoxelWorld(FVector(WX,WY,Z+200),FVector(0,0,-1),500,Hit,Previous))Z=Hit.Z+5;
        Job->Vertices.Add(FVector(WX-Job->Center.X,WY-Job->Center.Y,Z-Job->Center.Z));++Samples;
    }
    if(Job->Vertices.Num()==Side*Side){
        TArray<int32> Indices;Indices.Reserve(TileCells*TileCells*6);
        for(int X=0;X<TileCells;++X)for(int Y=0;Y<TileCells;++Y){const int I=X*Side+Y,J=I+Side;Indices.Append({I,I+1,J,I+1,J+1,J});}
        // One PMC per tile avoids re-cooking all previous sections each time.
        auto Mesh=Job->Sections[Job->Tile].Get();
        if(!Mesh){Mesh=NewObject<UProceduralMeshComponent>(A);Mesh->SetupAttachment(A->GetRootComponent());Mesh->SetMobility(EComponentMobility::Static);Mesh->bUseComplexAsSimpleCollision=true;
            Mesh->SetCollisionObjectType(ECC_WorldStatic);Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);Mesh->SetCollisionResponseToAllChannels(ECR_Block);Mesh->SetVisibility(false);Mesh->RegisterComponent();Job->Sections[Job->Tile]=Mesh;}
        Mesh->CreateMeshSection(0,Job->Vertices,Indices,TArray<FVector>(),TArray<FVector2D>(),TArray<FColor>(),TArray<FProcMeshTangent>(),true);
        Job->Vertices.Reset();Job->DirtyTiles.Remove(Job->Tile);if(Job->Tile==Job->NextTile)++Job->NextTile;
        if(Job->NextTile==TileCount&&Job->DirtyTiles.IsEmpty()){Job->Ready=true;A->Tags.Remove(TEXT("GroundRestorePartial"));UE_LOG(LogVoxelEarth,Log,TEXT("ObjectRestore GROUND_READY tiles=%d samplingMm=200 center=%s"),TileCount,*Job->Center.ToCompactString());}
    }
    return Job->Ready;
}
void NotifyTerrainEdited(UWorld* W,const FBox& Bounds){
    check(IsInGameThread());if(!W||!Bounds.IsValid||Bounds.Min.ContainsNaN()||Bounds.Max.ContainsNaN())return;
    auto Jobs=GroundRestores.Find(W);if(!Jobs)return;bool Changed=false;
    for(auto& Job:*Jobs){
        if(!Job->Actor.IsValid())continue;
        const double X0=Job->Center.X-Job->TilesX*160.,Y0=Job->Center.Y-Job->TilesY*160.;
        // Include a sampling-cell halo: an edited boundary vertex influences
        // triangles on both sides, including the adjacent collision section.
        if(Bounds.Max.X+20<X0||Bounds.Min.X-20>X0+Job->TilesX*320.||Bounds.Max.Y+20<Y0||Bounds.Min.Y-20>Y0+Job->TilesY*320.)continue;
        const int MinX=FMath::Clamp(FMath::FloorToInt((Bounds.Min.X-20-X0)/320.),0,Job->TilesX-1);
        const int MaxX=FMath::Clamp(FMath::FloorToInt((Bounds.Max.X+20-X0)/320.),0,Job->TilesX-1);
        const int MinY=FMath::Clamp(FMath::FloorToInt((Bounds.Min.Y-20-Y0)/320.),0,Job->TilesY-1);
        const int MaxY=FMath::Clamp(FMath::FloorToInt((Bounds.Max.Y+20-Y0)/320.),0,Job->TilesY-1);
        for(int X=MinX;X<=MaxX;++X)for(int Y=MinY;Y<=MaxY;++Y){
            const int Index=X*Job->TilesY+Y;
            // Unbuilt sections already sample the new terrain when reached.
            if(Index<Job->NextTile)Job->DirtyTiles.Add(Index);
            if(Index==Job->Tile)Job->Vertices.Reset();
        }
        Job->Ready=false;Job->Actor->Tags.AddUnique(TEXT("GroundRestorePartial"));Changed=true;
    }
    if(Changed)for(TActorIterator<AVoxelFallingTimber> It(W);It;++It){
        const FBox B=It->GetGroundSupportBounds();
        if(B.Min.X<=Bounds.Max.X+20&&B.Max.X>=Bounds.Min.X-20&&B.Min.Y<=Bounds.Max.Y+20&&B.Max.Y>=Bounds.Min.Y-20)It->PauseForGroundSupport();
    }
}
bool IsEquipped(UWorld* W){if(!W)return false;for(TActorIterator<AVoxelStoneAxePrototype> It(W);It;++It)if(It->Equipped)return true;return false;}
bool TrySwing(UWorld* W){for(TActorIterator<AVoxelStoneAxePrototype> It(W);It;++It)if(It->Equipped)return It->Swing();return false;}
void PrepareGround(UWorld* W,const FVector& Location){AdvanceRestoreGround(W,Location);}
void Prepare(UWorld* W,const FVector& Location){
    if(!W)return;PrepareGround(W,Location);
    EnsureAxe(W);
}
void Reset(UWorld* W){for(TActorIterator<AVoxelFallingTimber> It(W);It;++It)It->Destroy();}
}
namespace {
FAutoConsoleCommandWithWorld AxeToggle(TEXT("voxel.TreeFelling.Axe"),TEXT("Toggle the prototype stone axe; left click swings."),FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W){for(TActorIterator<AVoxelStoneAxePrototype> It(W);It;++It)It->Equipped=!It->Equipped;}));
}

