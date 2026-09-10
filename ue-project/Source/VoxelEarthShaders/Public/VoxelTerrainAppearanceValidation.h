#pragma once
#include "VoxelTerrainAppearanceUpload.h"
#include <bit>
// Strict canonical version3 admission shared by GT brick allocation and RT
// appearance publication. Legacy versions retain their existing admission rules.
inline bool VoxelValidateRecursiveAppearancePage(const FVoxelTerrainAppearanceUpload& U) {
    const auto& P=U.PageWords;
    if(!U.Sources||P.Num()<80||P.Num()>98304||P[0]!=3||U.Level<1||U.Level>7||P[5]!=U.Level||P[1]!=uint32(P.Num())||!U.Generation||
       int32(P[2])!=U.PageKey.X||int32(P[3])!=U.PageKey.Y||int32(P[4])!=U.PageKey.Z||P[13]!=uint32(U.Generation)||P[14]!=uint32(U.Generation>>32))return false;
    const uint32 Instances=P[6],Cells=P[7],Bricks=P[12];
    if(!Instances||Instances>4096||!Cells||Cells>32768||!Bricks||Bricks>64||P[8]!=16||P[9]!=80)return false;
    const uint64 InstanceStart=80u+uint64(Bricks)*17u,HandleStart=InstanceStart+uint64(Instances)*8u,OffsetStart=HandleStart+(uint64(Cells)+1u)/2u;
    if(P[10]!=InstanceStart||P[11]!=HandleStart||P[15]!=OffsetStart||OffsetStart+Cells!=uint64(P.Num()))return false;
    const auto& S=*U.Sources;
    if(S.SourceWords.IsEmpty()||uint64(S.SourceWords.Num())>64u*1024u*1024u||S.SourceRanges.Num()<2||S.SourceRanges.Num()>1048576||S.SourceRanges[0]!=FUintVector2(0,0))return false;
    for(const auto& R:S.SourceRanges)if(uint64(R.X)+R.Y>uint64(S.SourceWords.Num()))return false;
    uint32 NextBrick=1,Rank=0;
    for(uint32 B=0;B<64;++B){const uint32 D=P[16+B];if(!D)continue;if(D!=NextBrick||D>Bricks)return false;++NextBrick;
        const uint32 Base=80+(D-1)*17;if(P[Base]!=Rank)return false;uint32 Count=0;for(uint32 W=1;W<=16;++W)Count+=uint32(std::popcount(P[Base+W]));if(!Count||Count>Cells-Rank)return false;Rank+=Count;
    }
    if(NextBrick!=Bricks+1||Rank!=Cells)return false;
    for(uint32 I=0;I<Instances;++I){const uint32 Base=P[10]+I*8,Resource=P[Base];if(!Resource||Resource>=uint32(S.SourceRanges.Num())||P[Base+4]>3||P[Base+5]||P[Base+6]||P[Base+7])return false;if(S.SourceRanges[Resource].Y<20)return false;}
    bool Seen[4096]={};uint32 NextInstance=1;bool Nonzero=false;const int32 Scale=int32(1u<<U.Level),Half=Scale/2;
    for(uint32 C=0;C<Cells;++C){const uint32 Handle=(P[P[11]+C/2]>>((C&1u)*16u))&65535u;if(!Handle||Handle>Instances)return false;
        if(!Seen[Handle-1]){if(Handle!=NextInstance)return false;Seen[Handle-1]=true;++NextInstance;}
        const uint32 Offset=P[P[15]+C];if(Offset&0xff000000u)return false;Nonzero|=Offset!=0;
        for(uint32 Axis=0;Axis<3;++Axis){int32 Value=int32((Offset>>(Axis*8u))&255u);if(Value>=128)Value-=256;if(Value < -Half||Value>=Scale-Half)return false;}
    }
    if((Cells&1u)&&(P[P[11]+Cells/2]>>16u)!=0)return false;
    return Nonzero&&NextInstance==Instances+1;
}
