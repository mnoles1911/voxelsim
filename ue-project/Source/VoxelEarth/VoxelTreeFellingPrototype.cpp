#include "VoxelTreeFellingPrototype.h"
#include "Async/Async.h"
#include "VoxelDebrisLifecycle.h"
#include "VoxelDetachedPersistence.h"
#include "VoxelPackedTimberMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "VoxelWorldSubsystem.h"
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
    PrimaryActorTick.bCanEverTick=true;
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
void AVoxelFallingTimber::Tick(float Dt){
    Super::Tick(Dt);Age+=Dt;
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
    if(!StumpHinge&&!Breakable&&Age>35&&!Body->IsAnyRigidBodyAwake())SetActorTickEnabled(false);
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
    return SerializePersistentState(Ar,false);
}
bool AVoxelFallingTimber::SerializePersistentState(FArchive& Ar,bool GeometryOnly){
    using namespace VoxelDetachedPersistence;
    FTransform Transform=GetActorTransform(),HingeTransform=StumpHinge?StumpHinge->GetComponentTransform():FTransform::Identity;
    FVector Linear=Body->GetPhysicsLinearVelocity(),Angular=Body->GetPhysicsAngularVelocityInRadians();
    bool Hinged=StumpHinge!=nullptr,Awake=Body->IsAnyRigidBodyAwake();
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
        }
        Ar<<Relative<<Visible<<Cap;
        if(Ar.CustomVer(VoxelPackedTimberMesh::VersionKey)>=1){
            if(!VoxelPackedTimberMesh::Serialize(Ar,Section,SourceSection))return false;
        }else{
            if(Ar.IsSaving())Section=*SourceSection;
            if(!Array(Ar,Section.ProcVertexBuffer,262144,[](FArchive& A,FProcMeshVertex& V){A<<V.Position<<V.Normal<<V.UV0<<V.UV1<<V.Color<<V.Tangent.TangentX<<V.Tangent.bFlipTangentY;}))return false;
            if(!Array(Ar,Section.ProcIndexBuffer,393216,[](FArchive& A,uint32& Index){A<<Index;}))return false;
        }
        if(Ar.IsError()||Relative.ContainsNaN()||Cap>2||Section.ProcIndexBuffer.Num()%3)return false;
        if(Ar.IsLoading()){
            Section.SectionLocalBox=FBox(ForceInit);
            for(auto& V:Section.ProcVertexBuffer){if(V.Position.ContainsNaN()||V.Normal.ContainsNaN()||V.UV0.ContainsNaN()||V.UV1.ContainsNaN()||V.Tangent.TangentX.ContainsNaN())return false;Section.SectionLocalBox+=V.Position;}
            for(auto Index:Section.ProcIndexBuffer)if(Index>=uint32(Section.ProcVertexBuffer.Num()))return false;
            auto C=NewObject<UProceduralMeshComponent>(this);C->SetupAttachment(Body);C->RegisterComponent();
            C->SetWorldTransform(Relative*Transform);C->SetProcMeshSection(0,Section);C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            auto Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelEnvironmentLOD.M_VoxelEnvironmentLOD"));if(!Base)return false;
            auto Mat=UMaterialInstanceDynamic::Create(Base,this);Mat->SetScalarParameterValue(TEXT("WindEnabled"),0);Mat->SetScalarParameterValue(TEXT("Fade"),1);Mat->SetScalarParameterValue(TEXT("Reverse"),0);C->SetMaterial(0,Mat);
            if(Cap)C->ComponentTags.Add(Cap==1?TEXT("FractureLowerCap"):TEXT("FractureUpperCap"));
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
    FVector Linear=Body->GetPhysicsLinearVelocity(),Angular=Body->GetPhysicsAngularVelocityInRadians();
    bool Hinged=StumpHinge!=nullptr,Awake=Body->IsAnyRigidBodyAwake();
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
            if(D.IsError()||D.Tell()!=D.TotalSize()||!Job->Trunk.IsValid||Job->Trunk.GetSize().GetMin()<=0||Job->Trunk.GetSize().GetMax()>100000||Job->Transform.ContainsNaN()||Job->Linear.ContainsNaN()||Job->Angular.ContainsNaN()||Job->HingeTransform.ContainsNaN()||Job->Heading.ContainsNaN()||!FMath::IsFinite(Job->Age)||!FMath::IsFinite(Job->Lifetime.RemainingSeconds)||Kind>3)return false;
            Job->Lifetime.Kind=EVoxelDebrisLifetime(Kind);
            int32 Count=0;G<<Count;if(G.IsError()||Count<1||Count>4096)return false;
            Job->Sections.Reserve(Count);
            for(int I=0;I<Count;++I){
                if(Job->Cancelled.Load())return false;
                FTimberStagedRestore::FSection Section;G<<Section.Relative<<Section.Visible<<Section.Cap;
                if(G.IsError()||Section.Relative.ContainsNaN()||Section.Cap>2||!VoxelPackedTimberMesh::Serialize(G,Section.Mesh,nullptr))return false;
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
        if(S.Cap)C->ComponentTags.Add(S.Cap==1?TEXT("FractureLowerCap"):TEXT("FractureUpperCap"));
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
bool IsEquipped(UWorld* W){if(!W)return false;for(TActorIterator<AVoxelStoneAxePrototype> It(W);It;++It)if(It->Equipped)return true;return false;}
bool TrySwing(UWorld* W){for(TActorIterator<AVoxelStoneAxePrototype> It(W);It;++It)if(It->Equipped)return It->Swing();return false;}
void PrepareGround(UWorld* W,const FVector& Location){
    if(!W||W->GetNetMode()==NM_Client)return;
    for(TActorIterator<AActor> It(W);It;++It)if(It->ActorHasTag(TEXT("TreeFellingGround"))&&FVector::DistSquared(It->GetActorLocation(),Location)<FMath::Square(800.))return;
    auto Sub=W->GetSubsystem<UVoxelWorldSubsystem>();if(!Sub)return;
    auto Settings=UPhysicsSettings::Get();Settings->bSubstepping=true;Settings->MaxSubstepDeltaTime=1.f/120.f;Settings->MaxSubsteps=16;
    // Local collision bridge for this physics experiment. The production
    // terrain remains voxel-query-only. 20 cm sampling follows actual top cells.
    auto Ground=W->SpawnActor<AActor>();Ground->Tags.Add(TEXT("TreeFellingGround"));
    auto Mesh=NewObject<UProceduralMeshComponent>(Ground);Ground->SetRootComponent(Mesh);Ground->SetActorLocation(Location);
    Mesh->SetMobility(EComponentMobility::Static);Mesh->bUseComplexAsSimpleCollision=true;
    Mesh->SetCollisionObjectType(ECC_WorldStatic);Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Mesh->SetCollisionResponseToAllChannels(ECR_Block);Mesh->SetVisibility(false);Mesh->RegisterComponent();
    constexpr int N=160;constexpr double Step=20.;
    TArray<FVector> Vertices;TArray<int32> Indices;
    for(int X=0;X<=N;++X)for(int Y=0;Y<=N;++Y){
        const double WX=Location.X+(X-N/2)*Step,WY=Location.Y+(Y-N/2)*Step;
        double Z=Sub->GetSurfaceHeightUU(WX,WY);FVector Hit,Prev;
        if(Sub->RaycastVoxelWorld(FVector(WX,WY,Z+200),FVector(0,0,-1),500,Hit,Prev))Z=Hit.Z+5;
        Vertices.Add(FVector(WX-Location.X,WY-Location.Y,Z-Location.Z));
    }
    // PMC's collision provider flips normals: use Unreal's clockwise top-face
    // winding, matching the visible voxel mesher, so the surface blocks above.
    for(int X=0;X<N;++X)for(int Y=0;Y<N;++Y){const int A=X*(N+1)+Y,B=A+N+1;Indices.Append({A,A+1,B,A+1,B+1,B});}
    Mesh->CreateMeshSection(0,Vertices,Indices,TArray<FVector>(),TArray<FVector2D>(),TArray<FColor>(),TArray<FProcMeshTangent>(),true);
    UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling GROUND vertices=%d triangles=%d widthM=32 samplingMm=200"),Vertices.Num(),Indices.Num()/3);
}
void Prepare(UWorld* W,const FVector& Location){
    if(!W)return;PrepareGround(W,Location);
    for(TActorIterator<AVoxelStoneAxePrototype> It(W);It;++It)return;
    auto Axe=W->SpawnActor<AVoxelStoneAxePrototype>();if(!Axe->Initialize())Axe->Destroy();
}
void Reset(UWorld* W){for(TActorIterator<AVoxelFallingTimber> It(W);It;++It)It->Destroy();}
}
namespace {
FAutoConsoleCommandWithWorld AxeToggle(TEXT("voxel.TreeFelling.Axe"),TEXT("Toggle the prototype stone axe; left click swings."),FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W){for(TActorIterator<AVoxelStoneAxePrototype> It(W);It;++It)It->Equipped=!It->Equipped;}));
}
