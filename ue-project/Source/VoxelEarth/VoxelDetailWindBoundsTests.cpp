#include "VoxelDetailWindBounds.h"
#include "Math/Float16.h"
#include <limits>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDetailWindBoundsTest,"Voxel.Appearance.DetailWindBounds",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelDetailWindBoundsTest::RunTest(const FString&){
 using namespace VoxelDetailWindBounds;
 auto Mesh=[](float Class,float H,float Height){FVoxelDetailLodMesh M;M.Positions.Add(FVector3f(0,0,Height));M.Colors.Add(FVector4f(.1,.3,.1,Class));M.WindUVs.Add(FVector2f(H,Height));return M;};
 auto One=[](const FVoxelDetailLodMesh& M){const FVoxelDetailLodMesh* P=&M;return Calculate(MakeArrayView(&P,1));};
 for(float Class:{.25f,.5f,.75f,1.f,.6699f})for(float H:{0.f,.2f,.8f,1.f})for(float Height:{2.5f,20.f,37.5f,120.f,157.5f,400.f,100000.f}){
  auto M=Mesh(Class,H,Height);const auto B=One(M);TestTrue(TEXT("finite geometry derives bound"),B.Status==EStatus::Derived);TestEqual(TEXT("wind has no vertical displacement"),B.Extension.Z,0.f);
  const float PackedClass=FLinearColor(.1f,.3f,.1f,Class).ToFColor(true).A/255.f;
  const double Leaf=FMath::Abs(PackedClass-.5)<.08?1:0,Wood=FMath::Abs(PackedClass-.75)<.08?1:0,Soft=FMath::Abs(PackedClass-.25)<.08?1:0;
  for(double Time:{0.,.3,1.7,12.,101.})for(double Phase:{-1000.,-2.,0.,5.,1000.})for(FVector3d Wind:{FVector3d(0,0,0),FVector3d(8,2,1),FVector3d(-1.e6,1.e6,1.e6),FVector3d(1,0,-100)}){
   // Independent shader expression, including actual8bit metadata and halfUV.
   const double h=FMath::Clamp(double(FFloat16(H).GetFloat()),0.,1.);
   const double height=FMath::Max(double(FFloat16(Height).GetFloat()),1.);
   const double Speed=FMath::Sqrt(Wind.X*Wind.X+Wind.Y*Wind.Y);const FVector2d Dir(Wind.X/FMath::Max(Speed,.001),Wind.Y/FMath::Max(Speed,.001));
   const double Strength=FMath::Clamp((Speed+Wind.Z*.35)/10.,0.,1.);
   const double Slow=.62+.24*FMath::Sin(Time*1.15-Phase)+.14*FMath::Sin(Time*.53-Phase*.61);
   const double Bend=h*h*FMath::Min(24.,height*.06)*Slow,Flutter=(Leaf+Soft)*h*FMath::Min(3.,height*.015)*FMath::Sin(Time*3.7-Phase*2.3);
   const auto Offset=(Dir*Bend+FVector2d(-Dir.Y,Dir.X)*Flutter)*Strength*FMath::Min(1.,Leaf+Soft+Wood);
   TestTrue(TEXT("independent shader XY stays inside bound"),FMath::Abs(Offset.X)<=B.Extension.X+1.e-5&&FMath::Abs(Offset.Y)<=B.Extension.Y+1.e-5);
  }
 }
 auto Small=Mesh(.5f,1,20),Large=Mesh(.5f,1,200);const FVoxelDetailLodMesh* Chain[]={&Small,&Large};const auto All=Calculate(MakeArrayView(Chain));TestTrue(TEXT("all LOD vertices included"),All.Extension.X>=One(Large).Extension.X&&All.VerticesChecked==2);
 TestTrue(TEXT("small herb significantly below legacy30cm"),One(Small).Extension.X<1.3f);
 auto Inert=Mesh(1.f,1,400);TestTrue(TEXT("inert material needs no wind padding"),One(Inert).Extension.IsZero());
 auto Missing=Small;Missing.WindUVs.Reset();const auto Fallback=One(Missing);TestTrue(TEXT("missing data explicitly returns conservative legacy fallback"),Fallback.Status==EStatus::MissingGeometryFallback&&Fallback.Extension==FVector3f(30));
 auto Bad=Small;Bad.WindUVs[0].X=std::numeric_limits<float>::quiet_NaN();TestTrue(TEXT("NaN cannot masquerade as conservative fallback"),One(Bad).Status==EStatus::RejectNonFinite);Bad=Small;Bad.Colors[0].W=std::numeric_limits<float>::infinity();TestTrue(TEXT("infinite metadata explicitly rejected"),One(Bad).Status==EStatus::RejectNonFinite);
 Missing.Positions[0].X=std::numeric_limits<float>::quiet_NaN();TestTrue(TEXT("missing attributes do not conceal known nonfinite data"),One(Missing).Status==EStatus::RejectNonFinite);
 return !HasAnyErrors();
}
#endif
