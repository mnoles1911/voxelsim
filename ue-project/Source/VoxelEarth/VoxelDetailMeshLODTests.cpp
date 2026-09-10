#include "VoxelDetailMeshLOD.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
namespace {
void AddLodTestFace(FVoxelDetailLodMesh& G,int X,int Y,float Color=.4f,float Class=1.f){
 G.FaceMaterials.Add(uint8(Class));
 const uint32 B=G.Positions.Num();for(FVector2f D:{FVector2f(0,0),FVector2f(0,1),FVector2f(1,1),FVector2f(1,0)}){
  FVector3f P(X+D.X,Y+D.Y,0);G.Positions.Add(P);G.Normals.Add(FVector3f(0,0,1));G.TangentsX.Add(FVector3f(1,0,0));G.Colors.Add(FVector4f(Color,Color,Color,Class));G.UVs.Add(FVector2f(P.X*.1f,P.Y*.1f));G.WindUVs.Add(FVector2f(P.Y*.2f,.5f));
 }G.Indices.Append({B,B+1,B+2,B,B+2,B+3});
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDetailMeshLODTest,"Voxel.Appearance.DetailMeshLOD",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelDetailMeshLODTest::RunTest(const FString&){
 FVoxelDetailLodMesh G;for(int X=0;X<4;++X)for(int Y=0;Y<4;++Y)AddLodTestFace(G,X,Y,.4f+.002f*X);
 const auto Source=G;const auto L1=VoxelDetailMergeFaces(G,2,.015f),L2=VoxelDetailMergeFaces(G,4,.03f);
 TestEqual(TEXT("2x2 patches reduce32triangles to8"),L1.Indices.Num()/3,8);TestEqual(TEXT("4x4 patch reduces32triangles to2"),L2.Indices.Num()/3,2);
 TestTrue(TEXT("source geometry unchanged"),G.Positions==Source.Positions&&G.Colors==Source.Colors&&G.Indices==Source.Indices);
 for(int I=0;I<L2.Positions.Num();++I){const auto P=L2.Positions[I];TestTrue(TEXT("source planar UV retained"),L2.UVs[I].Equals(FVector2f(P.X*.1f,P.Y*.1f),1.e-6f));TestTrue(TEXT("affine wind retained"),L2.WindUVs[I].Equals(FVector2f(P.Y*.2f,.5f),1.e-6f));}
 for(const auto C:G.Colors)TestTrue(TEXT("merged color bounded"),FMath::Abs(C.X-L2.Colors[0].X)<=.03f);
 FVoxelDetailLodMesh Hole;AddLodTestFace(Hole,0,0);AddLodTestFace(Hole,1,0);AddLodTestFace(Hole,0,1);
 const auto HoleLOD=VoxelDetailMergeFaces(Hole,4,.03f);float HoleArea=0;bool FilledHole=false;for(int I=0;I<HoleLOD.Positions.Num();I+=4){const auto A=HoleLOD.Positions[I],C=HoleLOD.Positions[I+2];HoleArea+=(C.X-A.X)*(C.Y-A.Y);FilledHole|=A.X<1.5f&&C.X>1.5f&&A.Y<1.5f&&C.Y>1.5f;}TestEqual(TEXT("L-shaped face area retained"),HoleArea,3.f);TestFalse(TEXT("missing corner never bridged"),FilledHole);
 FVoxelDetailLodMesh Stem;for(int Y=0;Y<8;++Y)AddLodTestFace(Stem,0,Y);
 const auto StemLOD=VoxelDetailMergeFaces(Stem,8,.03f);TestEqual(TEXT("1x8 stem strip reduces to2triangles"),StemLOD.Indices.Num(),6);TestTrue(TEXT("stem endpoints retained"),StemLOD.Positions[0].Equals(FVector3f(0,0,0))&&StemLOD.Positions[2].Equals(FVector3f(1,8,0)));
 FVoxelDetailLodMesh Horizontal;for(int X=0;X<8;++X)AddLodTestFace(Horizontal,X,0);TestEqual(TEXT("8x1 horizontal strip reduces to2triangles"),VoxelDetailMergeFaces(Horizontal,8,.03f).Indices.Num(),6);
 const auto Repeat=VoxelDetailMergeFaces(G,4,.03f);TestTrue(TEXT("deterministic rectangle selection"),Repeat.Positions==L2.Positions&&Repeat.Colors==L2.Colors&&Repeat.Indices==L2.Indices);
 TestFalse(TEXT("weak tier omitted"),VoxelDetailLodWorthKeeping(100,90,.20f));TestTrue(TEXT("20percent boundary retained"),VoxelDetailLodWorthKeeping(100,80,.20f));TestFalse(TEXT("duplicate tier omitted"),VoxelDetailLodWorthKeeping(100,100,0.f));
 auto Boundary=G;Boundary.Colors[0].W=2;TestTrue(TEXT("material-class seam blocks affected patch"),VoxelDetailMergeFaces(Boundary,4,.03f).Indices.Num()>6);
 auto Material=G;Material.FaceMaterials[0]=99;TestTrue(TEXT("same-color material seam protected"),VoxelDetailMergeFaces(Material,4,.03f).Indices.Num()>6);
 auto Wind=G;Wind.WindUVs[0].X+=.1f;TestTrue(TEXT("nonaffine wind is protected"),VoxelDetailMergeFaces(Wind,4,.03f).Indices.Num()>6);
 auto UV=G;UV.UVs[0].X+=.1f;TestTrue(TEXT("UV seam protected"),VoxelDetailMergeFaces(UV,4,.03f).Indices.Num()>6);
 auto Color=G;Color.Colors[0].X=.9f;TestTrue(TEXT("color outlier protected"),VoxelDetailMergeFaces(Color,4,.03f).Indices.Num()>6);

 auto Missing=G;Missing.UVs.Reset();TestEqual(TEXT("malformed attributes fail open"),VoxelDetailMergeFaces(Missing,4,.03f).Indices.Num(),G.Indices.Num());
 auto Duplicate=G;AddLodTestFace(Duplicate,0,0);TestEqual(TEXT("duplicate face keys retain both"),VoxelDetailMergeFaces(Duplicate,4,.03f).Indices.Num(),Duplicate.Indices.Num());
 // Independent evaluation of shader WPO, comparing original and reduced
 // triangle interpolants over times, phases, heights and patch interiors.
 for(double Height:{10.,25.,100.,300.})for(double Size:{5.,10.})for(double Time:{0.,.7,3.1,12.})for(double Phase:{-2.,0.,2.4}){
  auto Wpo=[&](double X,double Y){const double H=Y/Height,P=Phase+X*.002;return FVector2d(H*H*FMath::Min(24.,Height*.06)*(.62+.24*FMath::Sin(Time*1.15-P)+.14*FMath::Sin(Time*.53-P*.61)),H*FMath::Min(3.,Height*.015)*FMath::Sin(Time*3.7-P*2.3));};
  auto Tri=[&](double X,double Y,double Step){const double OX=FMath::FloorToDouble(X/Step)*Step,OY=FMath::FloorToDouble(Y/Step)*Step,U=(X-OX)/Step,V=(Y-OY)/Step;const auto A=Wpo(OX,OY),B=Wpo(OX,OY+Step),C=Wpo(OX+Step,OY+Step),D=Wpo(OX+Step,OY);return V>=U?A*(1-V)+B*(V-U)+C*U:A*(1-U)+D*(U-V)+C*V;};
  const double Bound=VoxelDetailLodWindError(float(Height),float(1/Height),float(Size),2.5f,.5f);
  for(int X=1;X<10;++X)for(int Y=1;Y<10;++Y){const double PX=Size*X/10,PY=Size*Y/10;TestTrue(TEXT("sampled nonlinear WPO error below conservative bound"),(Tri(PX,PY,Size)-Tri(PX,PY,2.5)).Size()<=Bound+1.e-5);}
 }
 return !HasAnyErrors();
}
#endif
