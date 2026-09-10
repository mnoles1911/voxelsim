#pragma once
#include "CoreMinimal.h"
// Presentation-only coplanar reduction. No voxel/material occupancy changes.
struct FVoxelDetailLodMesh {
 TArray<FVector3f> Positions,Normals,TangentsX;
 TArray<FVector4f> Colors;
 TArray<FVector2f> UVs,WindUVs;
 TArray<uint32> Indices;
 TArray<uint8> FaceMaterials;
};
// Coupled to vegetation_material_common.py WIND_CODE. Conservative Hessian
// bound for coarse AND original triangular interpolants, in centimetres.
inline float VoxelDetailLodWindError(float Height,float HGradient,float PatchCm,float CellCm,float Class){
 const bool Leaf=FMath::Abs(Class-.5f)<.08f,Soft=FMath::Abs(Class-.25f)<.08f,Wood=FMath::Abs(Class-.75f)<.08f;
 if(!Leaf&&!Soft&&!Wood)return 0.f;
 const float A=FMath::Min(24.f,FMath::Max(Height,1.f)*.06f),C=(Leaf||Soft)?FMath::Min(3.f,FMath::Max(Height,1.f)*.015f):0.f;
 const float B=HGradient,K=.002f;
 const float Hessian=A*(2*B*B+4*B*K*.3254f+K*K*.292094f)+C*(2*B*K*2.3f+K*K*2.3f*2.3f);
 return Hessian*(PatchCm*PatchCm+CellCm*CellCm);
}
inline FVoxelDetailLodMesh VoxelDetailMergeFaces(const FVoxelDetailLodMesh& S,int MaxPatch,float ColorTolerance){
 FVoxelDetailLodMesh O;
 if(!FMath::IsFinite(ColorTolerance)||ColorTolerance<0)return S;
 const int Faces=S.Positions.Num()/4;
 if(S.Normals.Num()!=S.Positions.Num()||S.TangentsX.Num()!=S.Positions.Num()||S.Colors.Num()!=S.Positions.Num()||S.UVs.Num()!=S.Positions.Num()||S.WindUVs.Num()!=S.Positions.Num())return S;
 if(!Faces||S.Positions.Num()!=Faces*4||S.Indices.Num()!=Faces*6||S.FaceMaterials.Num()!=Faces)return S;
 const float Pitch=(S.Positions[1]-S.Positions[0]).Size();if(Pitch<=0)return S;
 TMap<FIntVector,int> Maps[6];TArray<FIntVector> Keys;TArray<int> Groups;Keys.SetNum(Faces);Groups.SetNum(Faces);
 for(int F=0;F<Faces;++F){const int B=F*4;const auto N=S.Normals[B];const int A=FMath::Abs(N.X)>.5f?0:FMath::Abs(N.Y)>.5f?1:2,U=(A+1)%3,V=(A+2)%3;
  if(!N.Equals(FVector3f(A==0?N[A]:0,A==1?N[A]:0,A==2?N[A]:0),1.e-6f)||!FMath::IsNearlyEqual(FMath::Abs(N[A]),1.f,1.e-6f))return S;
  for(int C=0;C<4;++C){const auto D=S.Positions[B+C]-S.Positions[B];const float EU=(C==2||C==3)?Pitch:0.f,EV=(C==1||C==2)?Pitch:0.f;if(!FMath::IsNearlyZero(D[A],1.e-5f)||!FMath::IsNearlyEqual(D[U],EU,1.e-5f)||!FMath::IsNearlyEqual(D[V],EV,1.e-5f))return S;}
  for(int I=0;I<6;++I)if(S.Indices[F*6+I]<uint32(B)||S.Indices[F*6+I]>=uint32(B+4))return S;
  Groups[F]=A*2+(N[A]>0?1:0);Keys[F]=FIntVector(FMath::RoundToInt(S.Positions[B][A]/Pitch),FMath::RoundToInt(S.Positions[B][U]/Pitch),FMath::RoundToInt(S.Positions[B][V]/Pitch));if(Maps[Groups[F]].Contains(Keys[F]))return S;Maps[Groups[F]].Add(Keys[F],F);
 }
 TBitArray<> Used(false,Faces);
 for(int F=0;F<Faces;++F){if(Used[F])continue;const int B=F*4,A=Groups[F]/2,U=(A+1)%3,V=(A+2)%3;const auto K=Keys[F];int ChosenW=1,ChosenH=1;FVector4f Color=S.Colors[B];
  const auto DU=(S.UVs[B+3]-S.UVs[B])/Pitch,DV=(S.UVs[B+1]-S.UVs[B])/Pitch;
  const auto WU=(S.WindUVs[B+3]-S.WindUVs[B])/Pitch,WV=(S.WindUVs[B+1]-S.WindUVs[B])/Pitch;
  // Deterministic maximum-area anchored rectangle; ties retain the first
  // width-ascending/height-ascending candidate. Every cell must be present.
  for(int W=1;W<=MaxPatch;++W)for(int H=1;H<=MaxPatch;++H){if(W*H<=ChosenW*ChosenH)continue;bool Good=true;FVector4f Mean(0,0,0,0);TArray<int> Members;
   for(int X=0;X<W&&Good;++X)for(int Y=0;Y<H;++Y){const int* P=Maps[Groups[F]].Find(K+FIntVector(0,X,Y));if(!P||Used[*P]){Good=false;break;}Members.Add(*P);Mean+=S.Colors[*P*4];}
   if(!Good)continue;Mean/=float(Members.Num());
   for(int Q:Members)for(int C=0;C<4;++C){const int I=Q*4+C;const auto D=S.Positions[I]-S.Positions[B];const auto RGB=S.Colors[I]-Mean;
    if(S.FaceMaterials[Q]!=S.FaceMaterials[F]||S.Colors[I].W!=S.Colors[B].W||FMath::Max3(FMath::Abs(RGB.X),FMath::Abs(RGB.Y),FMath::Abs(RGB.Z))>ColorTolerance||!S.Normals[I].Equals(S.Normals[B],1.e-6f)||!S.TangentsX[I].Equals(S.TangentsX[B],1.e-6f)||!S.UVs[I].Equals(S.UVs[B]+DU*D[U]+DV*D[V],1.e-5f)||!S.WindUVs[I].Equals(S.WindUVs[B]+WU*D[U]+WV*D[V],1.e-5f))Good=false;
   }
   if(!Good)continue;
   // Wind height must be constant; normalized root height may be affine.
   if(FMath::Abs(WU.Y)>1.e-6f||FMath::Abs(WV.Y)>1.e-6f||VoxelDetailLodWindError(S.WindUVs[B].Y,FMath::Sqrt(WU.X*WU.X+WV.X*WV.X),FMath::Max(W,H)*Pitch,Pitch,S.Colors[B].W)>.25f)continue;
   ChosenW=W;ChosenH=H;Color=Mean;
  }
  O.FaceMaterials.Add(S.FaceMaterials[F]);
  const uint32 Base=O.Positions.Num();for(int C=0;C<4;++C){auto P=S.Positions[B+C];P[U]=S.Positions[B][U]+(P[U]-S.Positions[B][U])*float(ChosenW);P[V]=S.Positions[B][V]+(P[V]-S.Positions[B][V])*float(ChosenH);const auto D=P-S.Positions[B];O.Positions.Add(P);O.Normals.Add(S.Normals[B+C]);O.TangentsX.Add(S.TangentsX[B+C]);O.Colors.Add(ChosenW*ChosenH==1?S.Colors[B+C]:Color);O.UVs.Add(ChosenW*ChosenH==1?S.UVs[B+C]:S.UVs[B]+DU*D[U]+DV*D[V]);O.WindUVs.Add(ChosenW*ChosenH==1?S.WindUVs[B+C]:S.WindUVs[B]+WU*D[U]+WV*D[V]);}
  for(int I=0;I<6;++I)O.Indices.Add(Base+(S.Indices[F*6+I]-uint32(B)));
  for(int X=0;X<ChosenW;++X)for(int Y=0;Y<ChosenH;++Y)Used[*Maps[Groups[F]].Find(K+FIntVector(0,X,Y))]=true;
 }
 return O;
}

inline bool VoxelDetailLodWorthKeeping(int32 PreviousIndices,int32 CandidateIndices,float MinimumSaving){
 return PreviousIndices>0&&CandidateIndices>0&&CandidateIndices<PreviousIndices&&float(PreviousIndices-CandidateIndices)/float(PreviousIndices)+1.e-6f>=MinimumSaving;
}
