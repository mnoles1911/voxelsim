#pragma once
#include "VoxelDetailMeshLOD.h"
// Shared by transient and persistent detail mesh builders.
// Contract: vegetation_material_common.py WIND_CODE; unit instance scale,
// quarter-yaw only. World/time/weather inputs must be finite. No Z displacement.
namespace VoxelDetailWindBounds {
enum class EStatus : uint8 { Derived, MissingGeometryFallback, RejectNonFinite };
struct FResult {
 EStatus Status=EStatus::MissingGeometryFallback;
 // Legacy fallback covers the shader's global finite-input amplitude ceiling.
 // Missing data does not certify input finiteness. NaN/Inf must be rejected,
 // never treated as safe merely because the legacy extension is retained.
 FVector3f Extension=FVector3f(30.f);
 int32 VerticesChecked=0;
};
inline FResult Calculate(TConstArrayView<const FVoxelDetailLodMesh*> Meshes){
 FResult R;bool Missing=Meshes.IsEmpty();double MaxXY=0;
 for(const auto* Mesh:Meshes){
  // Reject any known nonfinite data even if another attribute is missing.
  if(Mesh){
   for(const auto P:Mesh->Positions)if(!FMath::IsFinite(P.X)||!FMath::IsFinite(P.Y)||!FMath::IsFinite(P.Z)){R.Status=EStatus::RejectNonFinite;return R;}
   for(const auto W:Mesh->WindUVs)if(!FMath::IsFinite(W.X)||!FMath::IsFinite(W.Y)){R.Status=EStatus::RejectNonFinite;return R;}
   for(const auto C:Mesh->Colors)if(!FMath::IsFinite(C.X)||!FMath::IsFinite(C.Y)||!FMath::IsFinite(C.Z)||!FMath::IsFinite(C.W)){R.Status=EStatus::RejectNonFinite;return R;}
  }
  if(!Mesh||Mesh->Positions.IsEmpty()||Mesh->WindUVs.Num()!=Mesh->Positions.Num()||Mesh->Colors.Num()!=Mesh->Positions.Num()){Missing=true;continue;}
  for(int I=0;I<Mesh->Positions.Num();++I){const auto W=Mesh->WindUVs[I];const auto C=Mesh->Colors[I];
   ++R.VerticesChecked;
   // Color-buffer alpha quantizes to8bits. Cover either rounding convention
   // at class boundaries, rather than classifying unquantized source alpha.
   const double Class=FMath::Clamp(double(C.W),0.,1.),Radius=.08+1./255.;
   const bool Leaf=FMath::Abs(Class-.5)<=Radius,Soft=FMath::Abs(Class-.25)<=Radius,Wood=FMath::Abs(Class-.75)<=Radius;
   if(!Leaf&&!Soft&&!Wood)continue;
   // Account conservatively for half-precision UV storage as well as full
   // precision. At values large enough to overflow half, the shader's
   // saturate(h)/min(amplitude) still reach these same finite maxima.
   const double H0=FMath::Clamp(double(W.X),0.,1.);
   const double H=FMath::Min(1.,H0+FMath::Max(1.e-7,H0*.001));
   const double Height0=FMath::Max(1.,double(W.Y));
   const double Height=Height0+FMath::Max(.001,Height0*.001);
   const double A=H*H*FMath::Min(24.,Height*.06);
   const double B=(Leaf||Soft)?H*FMath::Min(3.,Height*.015):0.;
   // direction and its perpendicular are orthogonal, each length<=1.
   // slow=.62+.24*sin()+.14*sin() lies in[.24,1]; strength<=1.
   MaxXY=FMath::Max(MaxXY,FMath::Sqrt(A*A+B*B));
  }
 }
 if(Missing)return R;
 R.Status=EStatus::Derived;
 // .01cm (0.1mm) protects float rounding, without changing the amplitude model.
 const float Pad=MaxXY>0?float(MaxXY+.01):0.f;
 R.Extension=FVector3f(Pad,Pad,0.f);return R;
}
}
