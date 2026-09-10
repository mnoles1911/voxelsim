// Included by the existing test-only capture registration; not a bake input.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/PackageName.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDetailSizeCullCaptureTest,"Voxel.Appearance.DetailSizeCullRealAssets",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelDetailSizeCullCaptureTest::RunTest(const FString&){
 FString CasesPath,BakePath,BakeSHA,Out;
 if(!FParse::Value(FCommandLine::Get(),TEXT("VoxelDetailLodCases="),CasesPath)||!FParse::Value(FCommandLine::Get(),TEXT("VoxelDetailSizeCullBake="),BakePath)||!FParse::Value(FCommandLine::Get(),TEXT("VoxelDetailSizeCullBakeSHA="),BakeSHA)||!FParse::Value(FCommandLine::Get(),TEXT("VoxelDetailSizeCullCaptureOut="),Out)||!FParse::Param(FCommandLine::Get(),TEXT("VoxelDetailMeshLOD"))){AddError(TEXT("Requires LOD cases, size-cull bake path+SHA, fresh capture output, and -VoxelDetailMeshLOD"));return false;}
 if(IFileManager::Get().DirectoryExists(*Out)){AddError(TEXT("Capture output must be fresh"));return false;}
 auto ReadJson=[&](const FString& Path,TSharedPtr<FJsonObject>& J){FString T;return FFileHelper::LoadFileToString(T,*Path)&&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(T),J);};
 auto CheckFile=[&](const FString& Path,const FString& SHA){TArray<uint8> B;return FFileHelper::LoadFileToArray(B,*Path)&&DetailCaptureSha(B)==SHA.ToLower();};
 TSharedPtr<FJsonObject> Cases,Bake;
 if(!CheckFile(BakePath,BakeSHA)||!ReadJson(CasesPath,Cases)||!ReadJson(BakePath,Bake)||Bake->GetIntegerField(TEXT("schema"))!=2){AddError(TEXT("Invalid cases or hash-bound schema2 bake"));return false;}
 if(Cases->GetArrayField(TEXT("cases")).Num()!=4){AddError(TEXT("Exactly four source cases required"));return false;}
 bool SettingsOK=true;
 for(const auto Name:{TEXT("foliage.ForceLOD"),TEXT("foliage.OnlyLOD")}){const auto C=IConsoleManager::Get().FindConsoleVariable(Name);SettingsOK&=C&&C->GetInt()==-1;}
 for(const auto Name:{TEXT("foliage.DisableCull"),TEXT("foliage.CullAll"),TEXT("foliage.RandomLODRange"),TEXT("foliage.OverestimateLOD")}){const auto C=IConsoleManager::Get().FindConsoleVariable(Name);SettingsOK&=C&&C->GetFloat()==0;}
 const auto LODScale=IConsoleManager::Get().FindConsoleVariable(TEXT("foliage.LODDistanceScale"));SettingsOK&=LODScale&&LODScale->GetFloat()==1;
 if(!SettingsOK){AddError(TEXT("Requires normal automatic HISM LOD/culling settings; global overrides active"));return false;}
 auto Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelDetailAsset.M_VoxelDetailAsset"));
 auto Weather=LoadObject<UMaterialParameterCollection>(nullptr,TEXT("/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky"));
 if(!Base||!Weather||!CheckFile(FPackageName::LongPackageNameToFilename(TEXT("/Game/Voxel/M_VoxelDetailAsset"),TEXT(".uasset")),Bake->GetStringField(TEXT("material_source_sha256")))){AddError(TEXT("Actual base material missing or hash mismatch"));return false;}
 IFileManager::Get().MakeDirectory(*Out,true);
 FFileHelper::SaveStringToFile(BakePath+TEXT("\nsha256=")+BakeSHA+TEXT("\n1536px perspective FOV50/90, unit scale, ring256m, actual size policy. Albedo/depth only, not lighting/performance acceptance. Actual cached material parent with transient WindEnabled override; no asset edits.\n"),*(Out/TEXT("scope.txt")));
 FString Report=TEXT("id,path,fov,wind,time,pose,distance_cm,start_cm,end_cm,covered_pixels,changed_vs_cached,mean_rgb_error\n");
 int Completed=0;TSet<FString> Seen;
 const int Size=1536;
 for(const auto& CV:Cases->GetArrayField(TEXT("cases"))){const auto C=CV->AsObject();const FString Id=C->GetStringField(TEXT("id"));
  if(!(Id==TEXT("meadow-grass-0007")||Id==TEXT("meadow-daisy-0007")||Id==TEXT("water-reed-0007")||Id==TEXT("bramble-thicket-0007"))||Seen.Contains(Id)){AddError(TEXT("Exactly four distinct reviewed pilot cases required"));continue;}Seen.Add(Id);
  TSharedPtr<FJsonObject> Row;for(const auto& V:Bake->GetArrayField(TEXT("models")))if(V->AsObject()->GetStringField(TEXT("id"))==Id){Row=V->AsObject();break;}
  if(!Row||!CheckFile(Row->GetStringField(TEXT("package_file")),Row->GetStringField(TEXT("package_sha256")))||!CheckFile(Row->GetStringField(TEXT("material_file")),Row->GetStringField(TEXT("material_sha256")))||C->GetStringField(TEXT("vxa_sha256"))!=Row->GetStringField(TEXT("geometry_sha256"))||C->GetStringField(TEXT("vac_sha256"))!=Row->GetStringField(TEXT("appearance_sha256"))||!CheckFile(C->GetStringField(TEXT("vxa")),C->GetStringField(TEXT("vxa_sha256")))||!CheckFile(C->GetStringField(TEXT("vac")),C->GetStringField(TEXT("vac_sha256")))){AddError(Id+TEXT(" cache/source/material hash mismatch"));continue;}
  auto Cached=LoadObject<UStaticMesh>(nullptr,*Row->GetStringField(TEXT("object_path")));FAssetCompilingManager::Get().FinishAllCompilation();
  if(!Cached||!Cached->GetRenderData()||Cached->GetRenderData()->LODResources.Num()<2||!Cached->GetMaterial(0)||Cached->GetMaterial(0)->GetPathName()!=Row->GetStringField(TEXT("material_object_path"))){AddError(Id+TEXT(" cached mesh/material/LOD binding invalid"));continue;}
  TArray<uint8> V,B;FFileHelper::LoadFileToArray(V,*C->GetStringField(TEXT("vxa")));FFileHelper::LoadFileToArray(B,*C->GetStringField(TEXT("vac")));
  vxc::AssetGrid Grid;FString Error;if(Grid.parse(V.GetData(),V.Num())!=vxc::AssetParseError::kOk||Grid.voxelSizeMm()!=25){AddError(Id+TEXT(" invalid25mm source"));continue;}
  FMeshGeometry G;G.MeshKey=GetTypeHash(Id);G.Appearance=FVoxelAssetAppearance::Parse(MoveTemp(B),FMD5::HashBytes(V.GetData(),V.Num()).ToLower(),Error);if(!G.Appearance){AddError(Error);continue;}BuildNaiveFaceGeometry(Grid,G.MeshKey,G);auto Transient=CreateDetailStaticMesh(G,Base);if(!Transient){AddError(Id+TEXT(" transient build failed"));continue;}
  const auto CC=DetailSizeCullDistances(Cached->GetBounds(),FVector::OneVector,25600,true),TC=DetailSizeCullDistances(Transient->GetBounds(),FVector::OneVector,25600,true);
  TestFalse(Id+TEXT(" cached bounds policy fallback"),CC.bFallback);TestFalse(Id+TEXT(" transient bounds policy fallback"),TC.bFallback);TestEqual(Id+TEXT(" cached/transient start parity"),CC.StartUU,TC.StartUU);TestEqual(Id+TEXT(" cached/transient end parity"),CC.EndUU,TC.EndUU);
  TestTrue(Id+TEXT(" cached/transient wind bounds parity"),Cached->GetBounds().BoxExtent.Equals(Transient->GetBounds().BoxExtent,.001));TestEqual(Id+TEXT(" cached vertical wind extension zero"),Cached->GetPositiveBoundsExtension().Z,0.0);
  auto World=UWorld::CreateWorld(EWorldType::EditorPreview,false,FName(*FGuid::NewGuid().ToString()));if(!World){AddError(TEXT("World unavailable"));continue;}
  auto Actor=World->SpawnActor<AActor>();auto Hism=NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor);Actor->SetRootComponent(Hism);Actor->AddInstanceComponent(Hism);Hism->SetMobility(EComponentMobility::Movable);Hism->SetCollisionEnabled(ECollisionEnabled::NoCollision);Hism->SetForcedLodModel(0);Hism->RegisterComponent();
  auto CA=World->SpawnActor<AActor>();auto Capture=NewObject<USceneCaptureComponent2D>(CA);CA->SetRootComponent(Capture);CA->AddInstanceComponent(Capture);Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;Capture->ShowOnlyComponent(Hism);Capture->ShowFlags.SetTemporalAA(false);Capture->ShowFlags.SetAntiAliasing(false);Capture->ShowFlags.SetFog(false);Capture->ShowFlags.SetAtmosphere(false);Capture->RegisterComponent();
  auto Target=NewObject<UTextureRenderTarget2D>(CA);Target->RenderTargetFormat=RTF_RGBA32f;Target->ClearColor=FLinearColor::Black;Target->InitAutoFormat(Size,Size);Target->UpdateResourceImmediate(true);Capture->TextureTarget=Target;
  auto MPC=World->GetParameterCollectionInstance(Weather);if(!MPC||!MPC->SetScalarParameterValue(TEXT("WindFieldValid"),1)||!MPC->SetVectorParameterValue(TEXT("WindVectorMS"),FLinearColor(8,2,1,0))){AddError(TEXT("Weather pin failed"));World->DestroyWorld(false);continue;}World->UpdateParameterCollectionInstances(true,false);
  const FVector Center=Cached->GetBounds().Origin;const double Radius=Cached->GetBounds().SphereRadius,Margin=2*Radius+50;
  for(float Fov:{50.f,90.f})for(int Wind:{0,1}){
   Capture->FOVAngle=Fov;World->TimeSeconds=1.3f;World->RealTimeSeconds=1.3f;
   for(int Pose=0;Pose<4;++Pose){const double Distance=Pose==0?FMath::Max(200.,Radius*4):Pose==1?CC.EndUU-Margin:CC.EndUU+Margin;const bool Cull=Pose!=0&&Pose!=3;const TCHAR* PoseName=Pose==0?TEXT("near"):Pose==1?TEXT("inside"):Pose==2?TEXT("outside"):TEXT("outside_unculled");
    TArray<FLinearColor> RefColor,RefDepth;
    for(int Path=0;Path<2;++Path){auto Mesh=Path==0?Cached:Transient;Hism->ClearInstances();Hism->SetStaticMesh(Mesh);auto OriginalMaterial=Mesh->GetMaterial(0);auto Dynamic=Cast<UMaterialInstanceDynamic>(OriginalMaterial);if(!Dynamic)Dynamic=UMaterialInstanceDynamic::Create(OriginalMaterial,Hism);
     if(!Dynamic||Dynamic->GetMaterial()!=Base->GetMaterial()){AddError(Id+TEXT(" unexpected material parent; refusing capture"));continue;}
     bool ParametersMatch=true;for(const auto Name:{TEXT("TreeAppearance"),TEXT("TreeNeedle"),TEXT("FoliageCutout")}){float Original=-1,Actual=-2;ParametersMatch&=OriginalMaterial->GetScalarParameterValue(FMaterialParameterInfo(Name),Original)&&Dynamic->GetScalarParameterValue(FMaterialParameterInfo(Name),Actual)&&Original==Actual;}
     if(!ParametersMatch){AddError(Id+TEXT(" appearance parameters differ; refusing capture"));continue;}
     float SavedWind=1;Dynamic->GetScalarParameterValue(FMaterialParameterInfo(TEXT("WindEnabled")),SavedWind);Hism->SetMaterial(0,Dynamic);Dynamic->SetScalarParameterValue(TEXT("WindEnabled"),float(Wind));float ReadWind=-1;if(!Dynamic->GetScalarParameterValue(FMaterialParameterInfo(TEXT("WindEnabled")),ReadWind)||ReadWind!=float(Wind))AddError(TEXT("Wind override failed"));
     Hism->SetCullDistances(Cull?CC.StartUU:0,Cull?CC.EndUU:0);Hism->AddInstance(FTransform::Identity);Hism->BuildTreeIfOutdated(false,true);DetailCaptureMaterialReady(*this,Dynamic);
     const FVector Camera=Center+FVector(Distance,0,0);Capture->SetWorldLocation(Camera);Capture->SetWorldRotation((Center-Camera).Rotation());
     auto Read=[&](ESceneCaptureSource Source,TArray<FLinearColor>& Pixels){Capture->CaptureSource=Source;World->SendAllEndOfFrameUpdates();FlushRenderingCommands();Capture->CaptureScene();FlushRenderingCommands();FReadSurfaceDataFlags Flags(RCM_MinMax);Flags.SetLinearToGamma(false);return Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels,Flags)&&Pixels.Num()==Size*Size;};
     TArray<FLinearColor> Color,Depth;if(!Read(SCS_BaseColor,Color)||!Read(SCS_SceneDepth,Depth)){Dynamic->SetScalarParameterValue(TEXT("WindEnabled"),SavedWind);AddError(TEXT("Readback failed"));continue;}
     Dynamic->SetScalarParameterValue(TEXT("WindEnabled"),SavedWind);
     int Covered=0,Changed=0,Common=0;double RGBError=0;TArray<FLinearColor> Mask;Mask.Reserve(Depth.Num());
     const double MaxDepth=Distance+Radius*4+100;
     for(int I=0;I<Depth.Num();++I){const bool Hit=Depth[I].R>0&&Depth[I].R<MaxDepth;Covered+=Hit;Mask.Add(Hit?FLinearColor::White:FLinearColor::Black);if(Path&&RefDepth.Num()==Depth.Num()){const bool RH=RefDepth[I].R>0&&RefDepth[I].R<MaxDepth;Changed+=Hit!=RH;if(Hit&&RH){++Common;RGBError+=FMath::Abs(Color[I].R-RefColor[I].R)+FMath::Abs(Color[I].G-RefColor[I].G)+FMath::Abs(Color[I].B-RefColor[I].B);}}}
     if(Pose==2)TestEqual(Id+TEXT(" outside actual policy culls"),Covered,0);else TestTrue(Id+TEXT(" inside/unculled positive control visible"),Covered>0);
     if(Path&&Pose!=2){TestTrue(Id+TEXT(" cached/transient mask parity"),Changed<=FMath::Max(2,int(Covered*.02)));TestTrue(Id+TEXT(" cached/transient albedo parity"),Common>0&&RGBError/(3*Common)<.015);}
     const FString Stem=FString::Printf(TEXT("%s_%s_fov%d_wind%d_%s"),*Id,Path?TEXT("transient"):TEXT("cached"),int(Fov),Wind,PoseName);
     if(!DetailCapturePng(Out/(Stem+TEXT(".png")),Color,Size)||!DetailCapturePng(Out/(Stem+TEXT("_mask.png")),Mask,Size))AddError(TEXT("Image save failed"));
     Report+=FString::Printf(TEXT("%s,%s,%.0f,%d,1.3,%s,%.3f,%d,%d,%d,%d,%.8f\n"),*Id,Path?TEXT("transient"):TEXT("cached"),Fov,Wind,PoseName,Distance,CC.StartUU,CC.EndUU,Covered,Changed,Common?RGBError/(3*Common):0.);
     if(!Path){RefColor=MoveTemp(Color);RefDepth=MoveTemp(Depth);}
    }
   }
  }
  Hism->DestroyComponent();Capture->DestroyComponent();World->DestroyWorld(false);FlushRenderingCommands();++Completed;
 }
 TestEqual(TEXT("four cases completed"),Completed,4);if(!FFileHelper::SaveStringToFile(Report,*(Out/TEXT("size-cull-metrics.csv"))))AddError(TEXT("Metrics save failed"));return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDetailPersistentAuthoredTest,"Voxel.Appearance.DetailPersistentAuthoredLODs",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelDetailPersistentAuthoredTest::RunTest(const FString&){
 FString Path,Text;TSharedPtr<FJsonObject> Cases;
 if(!FParse::Value(FCommandLine::Get(),TEXT("VoxelDetailLodCases="),Path)||!FParse::Param(FCommandLine::Get(),TEXT("VoxelDetailMeshLOD"))||!FFileHelper::LoadFileToString(Text,*Path)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Cases)){AddError(TEXT("Requires hash-bound four-case LOD manifest and -VoxelDetailMeshLOD"));return false;}
 auto Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelDetailAsset.M_VoxelDetailAsset"));if(!Material){AddError(TEXT("Actual detail material missing"));return false;}
 int Completed=0;TSet<FString> Seen;
 for(const auto& CV:Cases->GetArrayField(TEXT("cases"))){const auto C=CV->AsObject();const FString Id=C->GetStringField(TEXT("id"));
  if(!(Id==TEXT("meadow-grass-0007")||Id==TEXT("meadow-daisy-0007")||Id==TEXT("water-reed-0007")||Id==TEXT("bramble-thicket-0007"))||Seen.Contains(Id)){AddError(TEXT("Unexpected/duplicate persistent LOD pilot case"));continue;}Seen.Add(Id);
  TArray<uint8> V,B;if(!FFileHelper::LoadFileToArray(V,*C->GetStringField(TEXT("vxa")))||!FFileHelper::LoadFileToArray(B,*C->GetStringField(TEXT("vac")))||DetailCaptureSha(V)!=C->GetStringField(TEXT("vxa_sha256"))||DetailCaptureSha(B)!=C->GetStringField(TEXT("vac_sha256"))){AddError(Id+TEXT(" hash mismatch"));continue;}
  vxc::AssetGrid Grid;FString Error;if(Grid.parse(V.GetData(),V.Num())!=vxc::AssetParseError::kOk||Grid.voxelSizeMm()!=25){AddError(Id+TEXT(" invalid source"));continue;}
  auto Appearance=FVoxelAssetAppearance::Parse(MoveTemp(B),FMD5::HashBytes(V.GetData(),V.Num()).ToLower(),Error);if(!Appearance){AddError(Error);continue;}
  FMeshGeometry Expected;Expected.Appearance=Appearance;BuildNaiveFaceGeometry(Grid,0,Expected);TestTrue(Id+TEXT(" includes authored reduced geometry"),!Expected.Lods.IsEmpty());
  auto Package=CreatePackage(*(TEXT("/Temp/DetailAuthored_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)));
  auto Mesh=VoxelBakePersistentDetailMesh(Grid,Appearance,Package,FName(TEXT("Mesh")),Material,Error);if(!Mesh){AddError(Id+TEXT(" ")+Error);continue;}
  const auto* Data=Mesh->GetRenderData();TestEqual(Id+TEXT(" retained source LOD count"),Data->LODResources.Num(),1+Expected.Lods.Num());
  for(int L=0;L<Data->LODResources.Num();++L){const auto& G=L?Expected.Lods[L-1]:static_cast<const FVoxelDetailLodMesh&>(Expected);
   TestEqual(Id+FString::Printf(TEXT(" authored LOD%d triangles"),L),Data->LODResources[L].GetNumTriangles(),uint32(G.Indices.Num()/3));
   TestFalse(Id+TEXT(" engine reduction disabled"),Mesh->IsReductionActive(L));TestEqual(Id+TEXT(" each source LOD owns itself"),Mesh->GetSourceModel(L).ReductionSettings.BaseLODModel,L);
  }
  ++Completed;
 }
 TestEqual(TEXT("all four source profiles tested"),Completed,4);return !HasAnyErrors();
}
#endif
