#pragma once
#include "CoreMinimal.h"

// Derived GPU word-buffer contract, not a disk format. All offsets are DWORDS.
// Header indices: [0]version [1]totalWords [2]records [3]bricks
// [4]dirOffset [5]maskOffset [6]dataOffset [7..9]sizeXYZ
// [10..12]originXYZ(signed) [13]pitchMm [14]needleFlag [15]reserved=0
// Version2: [15]bit0 explicitly enables foliage masks; version1 zero implies legacy tree masks.
// [16..19]sourceMD5, little-endian words from the verified VAC1 header.
// Directory: sorted lexicographic (brickX,brickY,brickZ,recordBase), four words.
// Masks: 16 words per directory entry, bit x+8*y+64*z. Data: one packed
// material|R<<8|G<<16|B<<24 word per set bit, ascending local bit order.
// Coordinates address the ORIGINAL source grid. Instance yaw belongs elsewhere.
struct FVoxelSparseAppearance
{
    static constexpr uint32 HeaderWords=20,Version=1;
    // Host-only conservative allocation requirements; excluded from word buffer.
    uint64 RequiredOutputBytes=0,RequiredWorkingBytes=0;
    TArray<uint32> Words;
    // Input is origin-inclusive; SourceCell output is zero-based, like VAC1 Sample.
    bool Lookup(const FIntVector& OriginInclusiveCell,uint8 Material,FColor& Color,FIntVector& SourceCell) const
    {
        if(Words.Num()<int32(HeaderWords)||(Words[0]!=1&&Words[0]!=2)||Words[1]!=uint32(Words.Num()))return false;
        const uint32 Pitch=Words[13];if(Words[0]==1?(Pitch!=25&&Pitch!=50&&Pitch!=100):(Pitch==0||Pitch>1000000))return false;
        const uint64 B=Words[3],N=Words[2];
        if(Words[4]!=HeaderWords||uint64(Words[5])!=HeaderWords+4*B||
           uint64(Words[6])!=HeaderWords+20*B||uint64(Words[6])+N!=uint64(Words.Num())||(Words[0]==1?Words[15]!=0:Words[15]>1))return false;
        int64 C[3];
        for(int A=0;A<3;++A){C[A]=int64(OriginInclusiveCell[A])-int32(Words[10+A]);if(C[A]<0||C[A]>=Words[7+A])return false;}
        const uint32 BX=uint32(C[0])>>3,BY=uint32(C[1])>>3,BZ=uint32(C[2])>>3;
        uint32 Lo=0,Hi=uint32(B);
        while(Lo<Hi){const uint32 Mid=Lo+(Hi-Lo)/2;const uint32* D=Words.GetData()+Words[4]+4*Mid;
            const bool Less=D[0]<BX||(D[0]==BX&&(D[1]<BY||(D[1]==BY&&D[2]<BZ)));
            if(Less)Lo=Mid+1;else Hi=Mid;
        }
        if(Lo>=B)return false;
        const uint32* D=Words.GetData()+Words[4]+4*Lo;if(D[0]!=BX||D[1]!=BY||D[2]!=BZ)return false;
        const uint32 Bit=(uint32(C[0])&7u)+8*(uint32(C[1])&7u)+64*(uint32(C[2])&7u),Word=Bit>>5,Shift=Bit&31u;
        const uint32* Mask=Words.GetData()+Words[5]+16*Lo;
        if((Mask[Word]&(1u<<Shift))==0)return false;
        uint32 Rank=0;for(uint32 I=0;I<Word;++I)Rank+=FMath::CountBits(Mask[I]);
        Rank+=FMath::CountBits(Mask[Word]&((1u<<Shift)-1u));
        const uint64 Record=uint64(D[3])+Rank;if(Record>=N)return false;
        const uint32 Packed=Words[int32(uint64(Words[6])+Record)];if(uint8(Packed)!=Material)return false;
        Color=FColor(uint8(Packed>>8),uint8(Packed>>16),uint8(Packed>>24));SourceCell=FIntVector(int32(C[0]),int32(C[1]),int32(C[2]));return true;
    }
};
