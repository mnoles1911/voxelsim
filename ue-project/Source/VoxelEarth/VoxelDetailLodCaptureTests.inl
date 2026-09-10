// Included inside the runtime detail translation unit for its actual mesh builder.
#if WITH_DEV_AUTOMATION_TESTS
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "ImageUtils.h"
#include "AssetCompilingManager.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialShared.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
namespace {
// RenderingCommands alone does not finish asynchronous material shader jobs.
// The first render-proxy lookup may enqueue missing permutations, so prime then
// drain compilation again before asserting that the real material will render.
bool DetailCaptureMaterialReady(FAutomationTestBase& Test,UMaterialInterface* Material){
 FAssetCompilingManager::Get().FinishAllCompilation();FlushRenderingCommands();
 const FMaterialRenderProxy* Proxy=Material->GetRenderProxy();
 ENQUEUE_RENDER_COMMAND(DetailLodPrimeMaterial)([Proxy](FRHICommandListImmediate&){const FMaterialRenderProxy* Fallback=nullptr;Proxy->GetMaterialWithFallback(GMaxRHIFeatureLevel,Fallback);});
 FlushRenderingCommands();FAssetCompilingManager::Get().FinishAllCompilation();FlushRenderingCommands();
 bool Ready=false;FString ActualName;
 ENQUEUE_RENDER_COMMAND(DetailLodCheckMaterial)([&](FRHICommandListImmediate&){const FMaterialRenderProxy* Used=nullptr;const FMaterial& Actual=Proxy->GetMaterialWithFallback(GMaxRHIFeatureLevel,Used);ActualName=Actual.GetFriendlyName();Ready=Used==nullptr&&Actual.IsRenderingThreadShaderMapComplete();});
 FlushRenderingCommands();Test.AddInfo(FString::Printf(TEXT("DetailLodCapture material=%s actual=%s noFallbackComplete=%d"),*Material->GetPathName(),*ActualName,Ready));return Test.TestTrue(TEXT("actual detail shader compiled without fallback"),Ready);
}
FString DetailCaptureSha(const TArray<uint8>& B){uint8 D[32];SHA256(B.GetData(),B.Num(),D);return BytesToHex(D,32).ToLower();}
bool DetailCapturePng(const FString& Path,const TArray<FLinearColor>& Pixels,int Size){TArray<FColor> C;C.Reserve(Pixels.Num());for(const auto P:Pixels)C.Add(P.ToFColor(true));TArray<uint8> Bytes;FImageUtils::CompressImageArray(Size,Size,C,Bytes);return FFileHelper::SaveArrayToFile(Bytes,*Path);}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDetailLodCaptureTest,"Voxel.Appearance.DetailLodRealAssets",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelDetailLodCaptureTest::RunTest(const FString&){
 FString Manifest,Out;
 if(!FParse::Value(FCommandLine::Get(),TEXT("VoxelDetailLodCases="),Manifest)||!FParse::Value(FCommandLine::Get(),TEXT("VoxelDetailLodCaptureOut="),Out)||!FParse::Param(FCommandLine::Get(),TEXT("VoxelDetailMeshLOD"))){AddError(TEXT("Requires -VoxelDetailMeshLOD -VoxelDetailLodCases=<cases.json> -VoxelDetailLodCaptureOut=<fresh output>"));return false;}
 if(IFileManager::Get().DirectoryExists(*Out)){AddError(TEXT("Use fresh capture output"));return false;}IFileManager::Get().MakeDirectory(*Out,true);
 FString Text;TSharedPtr<FJsonObject> Root;if(!FFileHelper::LoadFileToString(Text,*Manifest)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root)){AddError(TEXT("Invalid cases manifest"));return false;}
 if(!FFileHelper::SaveStringToFile(Text,*(Out/TEXT("source-cases.json")))){AddError(TEXT("Cannot save source bindings"));return false;}
 FFileHelper::SaveStringToFile(TEXT("Diagnostic forced static-mesh appearance comparisons plus separately marked automatic HISM selection/cull probes; not game performance acceptance.512x512 perspective50deg; trial bound screens .65/.10/.025. Real detail material and masks. Wind MPC=(8,2,1), valid1; WorldTime0/1.3. PNG albedo and binary scene-depth occupancy masks. Missing reduced LOD is a failure, not success. Review visual error before enabling.\n"),*(Out/TEXT("scope.txt")));
 auto Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelDetailAsset.M_VoxelDetailAsset"));auto Weather=LoadObject<UMaterialParameterCollection>(nullptr,TEXT("/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky"));if(!Material||!Weather){AddError(TEXT("Actual detail material/weather collection missing"));return false;}
 FString Report=TEXT("case,lod,vertices,triangles,screen,pose,wind,time,distance_cm,pixels,changed_coverage,rgb_abs_mean\n");int Cases=0,Reduced=0;
 const int Size=512;
 for(const auto Value:Root->GetArrayField(TEXT("cases"))){const auto Row=Value->AsObject();const FString Id=Row->GetStringField(TEXT("id"));TArray<uint8> V,B;
  if(!FFileHelper::LoadFileToArray(V,*Row->GetStringField(TEXT("vxa")))||!FFileHelper::LoadFileToArray(B,*Row->GetStringField(TEXT("vac")))||DetailCaptureSha(V)!=Row->GetStringField(TEXT("vxa_sha256"))||DetailCaptureSha(B)!=Row->GetStringField(TEXT("vac_sha256"))){AddError(Id+TEXT(" hash mismatch"));continue;}
  const auto MD5=FMD5::HashBytes(V.GetData(),V.Num()).ToLower();if(MD5!=Row->GetStringField(TEXT("vxa_md5"))){AddError(Id+TEXT(" MD5 mismatch"));continue;}
  vxc::AssetGrid Grid;FString Error;if(Grid.parse(V.GetData(),V.Num())!=vxc::AssetParseError::kOk||Grid.voxelSizeMm()!=25){AddError(Id+TEXT(" not valid25mm source"));continue;}
  FMeshGeometry G;G.MeshKey=GetTypeHash(Id);G.Appearance=FVoxelAssetAppearance::Parse(MoveTemp(B),MD5,Error);if(!G.Appearance){AddError(Error);continue;}BuildNaiveFaceGeometry(Grid,G.MeshKey,G);
  auto Mesh=CreateDetailStaticMesh(G,Material);FMeshGeometry Reference=G;Reference.Lods.Reset();auto Control=CreateDetailStaticMesh(Reference,Material);if(!Mesh||!Control){AddError(Id+TEXT(" mesh build failed"));continue;}
  const auto ExpectedWindBounds=DetailGeometryWindBounds(G);
  TestTrue(Id+TEXT(" actual source derives wind bounds"),ExpectedWindBounds.Status==VoxelDetailWindBounds::EStatus::Derived);
  TestTrue(Id+TEXT(" mesh positive extension matches all-LOD wind bound"),Mesh->GetPositiveBoundsExtension().Equals(FVector(ExpectedWindBounds.Extension),1.e-4));
  TestTrue(Id+TEXT(" mesh negative extension matches all-LOD wind bound"),Mesh->GetNegativeBoundsExtension().Equals(FVector(ExpectedWindBounds.Extension),1.e-4));
  TestEqual(Id+TEXT(" actual mesh adds no vertical wind padding"),Mesh->GetPositiveBoundsExtension().Z,0.0);
  const auto RawBounds=Mesh->GetRenderData()->Bounds;
  TestTrue(Id+TEXT(" extended mesh bounds include derived XY only"),Mesh->GetBounds().BoxExtent.Equals(FVector(RawBounds.BoxExtent)+FVector(ExpectedWindBounds.Extension),1.e-3));

  if(!DetailCaptureMaterialReady(*this,Mesh->GetStaticMaterials()[0].MaterialInterface)||!DetailCaptureMaterialReady(*this,Control->GetStaticMaterials()[0].MaterialInterface))continue;
  ++Cases;const auto* Data=Mesh->GetRenderData();if(Data->LODResources.Num()<2){AddError(Id+TEXT(" no actual reduced LOD; pilot not successful for this asset"));}else ++Reduced;
  auto World=UWorld::CreateWorld(EWorldType::EditorPreview,false,FName(*FGuid::NewGuid().ToString()));if(!World){AddError(TEXT("No capture world"));continue;}
  auto Actor=World->SpawnActor<AActor>();auto Component=NewObject<UStaticMeshComponent>(Actor);Actor->SetRootComponent(Component);Actor->AddInstanceComponent(Component);Component->SetStaticMesh(Mesh);Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);Component->RegisterComponent();
  auto CameraActor=World->SpawnActor<AActor>();auto Capture=NewObject<USceneCaptureComponent2D>(CameraActor);CameraActor->SetRootComponent(Capture);CameraActor->AddInstanceComponent(Capture);Capture->FOVAngle=50;Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->bAlwaysPersistRenderingState=true;Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;Capture->ShowOnlyComponent(Component);Capture->ShowFlags.SetTemporalAA(false);Capture->ShowFlags.SetAntiAliasing(false);Capture->ShowFlags.SetFog(false);Capture->ShowFlags.SetAtmosphere(false);Capture->RegisterComponent();
  auto Target=NewObject<UTextureRenderTarget2D>(CameraActor);Target->RenderTargetFormat=RTF_RGBA32f;Target->ClearColor=FLinearColor::Black;Target->InitAutoFormat(Size,Size);Target->UpdateResourceImmediate(true);Capture->TextureTarget=Target;
  auto Params=World->GetParameterCollectionInstance(Weather);if(!Params||!Params->SetScalarParameterValue(TEXT("WindFieldValid"),1)||!Params->SetVectorParameterValue(TEXT("WindVectorMS"),FLinearColor(8,2,1,0))){AddError(TEXT("Cannot pin actual weather inputs"));World->DestroyWorld(false);continue;}
  World->UpdateParameterCollectionInstances(true,false);FlushRenderingCommands();
  const auto Bounds=Mesh->GetBounds();const FVector Center(Bounds.Origin);const double Radius=Bounds.SphereRadius;
  auto Read=[&](ESceneCaptureSource Source,TArray<FLinearColor>& Pixels){Capture->CaptureSource=Source;World->SendAllEndOfFrameUpdates();FlushRenderingCommands();Capture->CaptureScene();FlushRenderingCommands();FReadSurfaceDataFlags Flags(RCM_MinMax);Flags.SetLinearToGamma(false);return Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels,Flags)&&Pixels.Num()==Size*Size;};
  TArray<FLinearColor> WindOffNear,WindOffDepth;int WindChanged=0;
  for(int Wind=0;Wind<2;++Wind)for(double Time:{0.,1.3})for(int Pose=0;Pose<3;++Pose){World->TimeSeconds=Time;World->RealTimeSeconds=Time;World->UnpausedTimeSeconds=Time;
   for(auto M:{Mesh,Control}){auto Dynamic=Cast<UMaterialInstanceDynamic>(M->GetStaticMaterials()[0].MaterialInterface);if(!Dynamic){AddError(TEXT("Actual appearance dynamic material missing"));continue;}Dynamic->SetScalarParameterValue(TEXT("WindEnabled"),float(Wind));float ReadWind=-1;if(!Dynamic->GetScalarParameterValue(FMaterialParameterInfo(TEXT("WindEnabled")),ReadWind)||ReadWind!=float(Wind))AddError(TEXT("Actual WindEnabled parameter did not pin"));}
   const double Screen=Pose==0?.65:Pose==1?.10:.025,Distance=Radius/FMath::Tan(FMath::DegreesToRadians(25.))/Screen;
   const FVector Direction=FVector(1,-1,.35).GetSafeNormal();Capture->SetWorldLocation(Center+Direction*Distance);Capture->SetWorldRotation((-Direction).Rotation());
   TArray<FLinearColor> Ref,RefDepth;Component->SetStaticMesh(Control);Component->SetForcedLodModel(1);if(!Read(SCS_BaseColor,Ref)||!Read(SCS_SceneDepth,RefDepth)){AddError(TEXT("Control readback failed"));continue;}
   if(Wind==0&&Time==0&&Pose==0){int Green=0;for(int I=0;I<Ref.Num();++I)if(RefDepth[I].R>0&&RefDepth[I].R<Distance+Radius*4&&Ref[I].G>Ref[I].R+.02f&&Ref[I].G>Ref[I].B+.02f)++Green;TestTrue(Id+TEXT(" real green source albedo visible; gray fallback cannot pass"),Green>0);AddInfo(FString::Printf(TEXT("DetailLodCapture %s greenReferencePixels=%d"),*Id,Green));}
   const FString Prefix=FString::Printf(TEXT("%s_p%d_w%d_t%d"),*Id,Pose,Wind,int(Time*10));if(!DetailCapturePng(Out/TEXT("control_")+Prefix+TEXT(".png"),Ref,Size))AddError(TEXT("Control PNG save failed"));
   for(int L=0;L<Data->LODResources.Num();++L){Component->SetStaticMesh(Mesh);Component->SetForcedLodModel(L+1);TArray<FLinearColor> Pixels,Depth;if(!Read(SCS_BaseColor,Pixels)||!Read(SCS_SceneDepth,Depth)){AddError(TEXT("LOD readback failed"));continue;}
    if(L==0&&Pose==0&&Time==0){if(Wind==0){WindOffNear=Pixels;WindOffDepth=Depth;}else if(WindOffNear.Num()==Pixels.Num())for(int I=0;I<Pixels.Num();++I){if(!Pixels[I].Equals(WindOffNear[I],1.e-5f)||!FMath::IsNearlyEqual(Depth[I].R,WindOffDepth[I].R,.001f))++WindChanged;}}
    int Covered=0,Changed=0;double Difference=0;TArray<FLinearColor> Mask;Mask.Reserve(Pixels.Num());for(int I=0;I<Pixels.Num();++I){const bool A=Depth[I].R>0&&Depth[I].R<Distance+Radius*4,Bg=RefDepth[I].R>0&&RefDepth[I].R<Distance+Radius*4;Covered+=A;Changed+=A!=Bg;Mask.Add(A?FLinearColor::White:FLinearColor::Black);if(A&&Bg)Difference+=FMath::Abs(Pixels[I].R-Ref[I].R)+FMath::Abs(Pixels[I].G-Ref[I].G)+FMath::Abs(Pixels[I].B-Ref[I].B);}
    if(Covered==0)AddError(Id+TEXT(" empty asset capture"));if(L==0&&(Changed||Difference>1.e-4))AddError(Id+TEXT(" runtime LOD0 differs from unchanged source control"));
    const FString Name=Prefix+FString::Printf(TEXT("_lod%d"),L);if(!DetailCapturePng(Out/Name+TEXT(".png"),Pixels,Size)||!DetailCapturePng(Out/Name+TEXT("_mask.png"),Mask,Size))AddError(TEXT("PNG save failed"));
    const auto& R=Data->LODResources[L];Report+=FString::Printf(TEXT("%s,%d,%u,%u,%.4f,%d,%d,%.3f,%.4f,%d,%d,%.8f\n"),*Id,L,R.GetNumVertices(),R.GetNumTriangles(),Data->ScreenSize[L].Default,Pose,Wind,Time,Distance,Covered,Changed,Difference/FMath::Max(1,Covered*3));
   }
  }
  TestTrue(Id+TEXT(" actual near wind-on/off pixels or depth must differ"),WindChanged>0);AddInfo(FString::Printf(TEXT("DetailLodCapture %s windChangedPixels=%d"),*Id,WindChanged));
  // Diagnostic clone: only RGB is replaced with an unambiguous per-LOD
  // marker. Geometry, masks, UVs, alpha classes and transition screens match.
  // This is actual automatic HISM rendering, not predicted selection from range.
  FMeshGeometry Marked=G;Marked.MeshKey^=0x31415926u;
  auto Paint=[](FVoxelDetailLodMesh& Geometry,int L){for(auto& C:Geometry.Colors){C.X=L==0?.8f:.02f;C.Y=L==1?.8f:.02f;C.Z=L>=2?.8f:.02f;}};
  Paint(Marked,0);for(int L=0;L<Marked.Lods.Num();++L)Paint(Marked.Lods[L],L+1);
  auto MarkerMesh=CreateDetailStaticMesh(Marked,Material);
  if(MarkerMesh&&DetailCaptureMaterialReady(*this,MarkerMesh->GetStaticMaterials()[0].MaterialInterface)){
   auto MarkerMaterial=Cast<UMaterialInstanceDynamic>(MarkerMesh->GetStaticMaterials()[0].MaterialInterface);if(!MarkerMaterial){AddError(TEXT("Marker appearance material missing"));}else MarkerMaterial->SetScalarParameterValue(TEXT("WindEnabled"),0);
   bool SettingsOK=true;for(const auto Name:{TEXT("foliage.ForceLOD"),TEXT("foliage.OnlyLOD")}){const auto C=IConsoleManager::Get().FindConsoleVariable(Name);SettingsOK&=C&&C->GetInt()==-1;}
   for(const auto Name:{TEXT("foliage.DisableCull"),TEXT("foliage.CullAll"),TEXT("foliage.RandomLODRange"),TEXT("foliage.OverestimateLOD")}){const auto C=IConsoleManager::Get().FindConsoleVariable(Name);SettingsOK&=C&&C->GetFloat()==0;}
   const auto ScaleVar=IConsoleManager::Get().FindConsoleVariable(TEXT("foliage.LODDistanceScale"));SettingsOK&=ScaleVar&&ScaleVar->GetFloat()==1;
   TestTrue(TEXT("automatic HISM diagnostic requires default unforced/unrandomized culling"),SettingsOK);
   auto Hism=NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor);Actor->AddInstanceComponent(Hism);Hism->SetStaticMesh(MarkerMesh);Hism->SetMobility(EComponentMobility::Movable);Hism->SetCollisionEnabled(ECollisionEnabled::NoCollision);Hism->SetForcedLodModel(0);Hism->SetCullDistances(0,0);Hism->RegisterComponent();Hism->AddInstance(FTransform::Identity);Hism->BuildTreeIfOutdated(false,true);FlushRenderingCommands();SettingsOK&=DetailCaptureMaterialReady(*this,MarkerMesh->GetStaticMaterials()[0].MaterialInterface);
   Capture->ClearShowOnlyComponents();Capture->ShowOnlyComponent(Hism);Capture->bAlwaysPersistRenderingState=false;
   TArray<FClusterNode> Nodes;Hism->GetTree(Nodes);SettingsOK&=TestEqual(TEXT("single-instance HISM has one actual cluster"),Nodes.Num(),1);
   const float WpoExtent=MarkerMesh->GetStaticMaterials()[0].MaterialInterface->GetMaxWorldPositionOffsetDisplacement();
   const FVector NodeMin=Nodes.IsEmpty()?FVector::ZeroVector:FVector(Nodes[0].BoundMin)-FVector(WpoExtent),NodeMax=Nodes.IsEmpty()?FVector::ZeroVector:FVector(Nodes[0].BoundMax)+FVector(WpoExtent);
   const FVector NodeCenter=(NodeMin+NodeMax)*.5;const double NodeHalfDiagonal=(NodeMax-NodeMin).Size()*.5;
   SettingsOK&=TestFalse(TEXT("current diagnostic expects nondithered detail material"),MarkerMesh->GetStaticMaterials()[0].MaterialInterface->IsDitheredLODTransition());
   const auto* MarkerData=MarkerMesh->GetRenderData();const double LodRadius=MarkerData->Bounds.SphereRadius+MarkerMesh->GetStaticMaterials()[0].MaterialInterface->GetMaxWorldPositionOffsetDisplacement();
   FString AutoReport=TEXT("probe,distance_cm,expected_lod,red_pixels,green_pixels,blue_pixels\n");
   auto Observe=[&](const FString& Name,double Distance,int Expected){
    const FVector Direction=FVector(1,-1,.35).GetSafeNormal();Capture->SetWorldLocation(NodeCenter+Direction*Distance);Capture->SetWorldRotation((-Direction).Rotation());
    TArray<FLinearColor> Pixels;if(!Read(SCS_BaseColor,Pixels)){AddError(TEXT("HISM automatic readback failed"));return 0;}int Counts[3]={0,0,0};
    for(const auto C:Pixels){if(C.R>C.G+.1f&&C.R>C.B+.1f)++Counts[0];else if(C.G>C.R+.1f&&C.G>C.B+.1f)++Counts[1];else if(C.B>C.R+.1f&&C.B>C.G+.1f)++Counts[2];}
    const int Total=Counts[0]+Counts[1]+Counts[2];if(Expected>=0){TestTrue(Id+Name+TEXT(" expected actual LOD marker visible"),Counts[Expected]>0);for(int L=0;L<3;++L)if(L!=Expected)TestEqual(Id+Name+TEXT(" no incorrect LOD marker"),Counts[L],0);}
    if(!DetailCapturePng(Out/(Id+TEXT("_hism_")+Name+TEXT(".png")),Pixels,Size))AddError(TEXT("HISM PNG save failed"));
    AutoReport+=FString::Printf(TEXT("%s,%.5f,%d,%d,%d,%d\n"),*Name,Distance,Expected,Counts[0],Counts[1],Counts[2]);return Total;
   };
   if(SettingsOK){for(int L=1;L<MarkerData->LODResources.Num();++L){const double Boundary=LodRadius/FMath::Tan(FMath::DegreesToRadians(25.))/MarkerData->ScreenSize[L].Default;// Preserve nominal screen probes as observations, but test the actual
     // UE nondithered cluster predicate: DistCenter-HalfDiagonal > Boundary.
     Observe(FString::Printf(TEXT("nominal_before_lod%d"),L),Boundary*.9,-1);Observe(FString::Printf(TEXT("nominal_after_lod%d"),L),Boundary*1.1,-1);
     const double Margin=FMath::Max(5.,Boundary*.05),Switch=Boundary+NodeHalfDiagonal;
     AddInfo(FString::Printf(TEXT("HISM bounds %s lod%d renderRadius=%.5f nodeHalfDiagonal=%.5f nominalBoundary=%.5f actualMinLodSwitch=%.5f"),*Id,L,LodRadius,NodeHalfDiagonal,Boundary,Switch));
     Observe(FString::Printf(TEXT("before_lod%d"),L),Switch-Margin,L-1);Observe(FString::Printf(TEXT("after_lod%d"),L),Switch+Margin,L);}
    // The shrub remains resolvable at48m in perspective. Tiny herbs are NOT
    // counted as successful culls from an already-empty subpixel image.
    if(Id.StartsWith(TEXT("bramble-thicket"))){const double Outside=4800+Radius*2,Inside=4800-Radius*2;Hism->SetCullDistances(4080,4800);const int InsidePixels=Observe(TEXT("inside_cull"),Inside,-1);TestTrue(TEXT("inside48m shrub remains visible"),InsidePixels>0);const int Hidden=Observe(TEXT("outside_cull"),Outside,-1);Hism->SetCullDistances(0,0);const int ControlPixels=Observe(TEXT("outside_no_cull_control"),Outside,-1);TestEqual(TEXT("HISM outside48m culls marker pixels"),Hidden,0);TestTrue(TEXT("same far perspective pose without cull remains visible"),ControlPixels>0);}
   }
   if(!FFileHelper::SaveStringToFile(AutoReport,*(Out/(Id+TEXT("_hism-selection.csv")))))AddError(TEXT("HISM metrics save failed"));Hism->DestroyComponent();
  }else AddError(TEXT("Marker mesh unavailable"));
  Component->DestroyComponent();Capture->DestroyComponent();World->DestroyWorld(false);FlushRenderingCommands();
 }
 if(!FFileHelper::SaveStringToFile(Report,*(Out/TEXT("capture-metrics.csv"))))AddError(TEXT("Metrics save failed"));TestEqual(TEXT("four real asset types exercised"),Cases,4);TestEqual(TEXT("all four have actual reduced geometry"),Reduced,4);return !HasAnyErrors();
}
#include "VoxelDetailSizeCullCaptureTests.inl"
#endif
