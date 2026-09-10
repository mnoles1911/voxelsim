#include "VoxelDebrisCapture.h"
#include "voxelcore/worldquery.h"
#include "voxelcore/assetcandidate.h"

bool VoxelDebrisCapture::Capture(const vxc::World<8>& World,const FVoxelAppearanceBankBinding* Binding,
    const TArray<VoxelCoords::FVoxelCoord>& Cells,TArray<FVoxelDebrisCellAppearance>& Out)
{
    Out.Reset();if(Cells.IsEmpty()||Cells.Num()>512*1024)return false;
    constexpr int64 Limit=MAX_int64/vxc::kVoxelSizeMm-MAX_int32;
    vxc::AssetVoxelRect Rect{Cells[0].X,Cells[0].Y,Cells[0].X,Cells[0].Y};
    TSet<VoxelCoords::FVoxelCoord> Needed,Touched;Needed.Reserve(Cells.Num());
    for(const auto& V:Cells){
        if(V.X < -Limit||V.X>Limit||V.Y < -Limit||V.Y>Limit||V.Z==MIN_int64||V.Z==MAX_int64||Needed.Contains(V))return false;
        Needed.Add(V);Rect.vx0=FMath::Min(Rect.vx0,V.X);Rect.vx1=FMath::Max(Rect.vx1,V.X);Rect.vy0=FMath::Min(Rect.vy0,V.Y);Rect.vy1=FMath::Max(Rect.vy1,V.Y);
    }
    auto MarkLog=[&](const vxc::EditLog& Log,bool Craft){for(const auto& Entry:Log.entries())for(const auto& C:Entry.cells){
        int64 X=int64(Entry.key.x)*8+C.cell%8,Y=int64(Entry.key.y)*8+(C.cell/8)%8,Z=int64(Entry.key.z)*8+C.cell/64;
        if(Craft){X=World.voxelOfCraftCell(X);Y=World.voxelOfCraftCell(Y);Z=World.voxelOfCraftCell(Z);}
        const VoxelCoords::FVoxelCoord V{X,Y,Z};if(Needed.Contains(V))Touched.Add(V);
    }};
    MarkLog(World.log(),false);MarkLog(World.craftLog(),true);
    vxc::WorldQuery<8> Query(World,Rect);
    const auto Catalog=Binding?Binding->SourceSnapshot():nullptr;
    struct FEntry {int64 X,Y;int32 Radius;std::vector<vxc::AssetField::ResolvedAssetInstance> Resolved;};
    TMap<FIntPoint,std::vector<FEntry>> Footprints;
    Out.SetNum(Cells.Num());
    for(int I=0;I<Cells.Num();++I){const auto& V=Cells[I];auto& D=Out[I];D.Coord=V;D.Material=uint8(Query.materialAt(V.X,V.Y,V.Z));
        // Detectors only hand off solid cells. A stale/air snapshot must not
        // remove the island then create an empty or misidentified body.
        if(!D.Material){Out.Reset();return false;}
        const auto* Field=World.assetField();
        if(!Catalog||!Field||Touched.Contains(V)||World.amplifier().materialAt(V.X,V.Y,V.Z)!=vxc::MAT_AIR)continue;
        const int64 PX=vxc::floorDiv(V.X,32),PY=vxc::floorDiv(V.Y,32);
        if(PX<MIN_int32||PX>MAX_int32||PY<MIN_int32||PY>MAX_int32)continue;
        const FIntPoint Key{int32(PX),int32(PY)};auto* Entries=Footprints.Find(Key);
        if(!Entries){std::vector<FEntry> Prepared;
            const auto Instances=Field->instancesForRect({PX*32,PY*32,PX*32+31,PY*32+31},[&](int64 X,int64 Y){const auto Column=World.amplifier().columnCached(X,Y);return vxc::assetColumnFactsFromSample(Column,World.assetChannelsAt(X,Y));});
            for(const auto& Instance:Instances){auto Resolved=Field->resolveForCompose({Instance});if(Resolved.empty())continue;
                Prepared.push_back({Instance.anchorXMm,Instance.anchorYMm,Field->layers()[Instance.layer].maxRadiusMm,MoveTemp(Resolved)});}
            Entries=&Footprints.Add(Key,MoveTemp(Prepared));
        }
        bool Winner=false;const int64 X0=V.X*100,Y0=V.Y*100;
        for(const auto& Entry:*Entries){
            // A footprint query is a superset of the authoritative point query.
            // Apply its reach gate BEFORE choosing any source, including an
            // unapproved earlier source of the same material.
            if(Entry.Radius<0||Entry.X+Entry.Radius<X0||Entry.X-Entry.Radius>X0+99||Entry.Y+Entry.Radius<Y0||Entry.Y-Entry.Radius>Y0+99)continue;
            for(const auto& Instance:Entry.Resolved){
                vxc::AssetCandidateBounds Bounds;if(!vxc::assetCandidateBounds(Instance,Bounds)){Winner=true;break;}
                const auto M=vxc::assetCandidateMaterial(Instance,Bounds,V.X,V.Y,V.Z);if(M==vxc::MAT_AIR)continue;
                Winner=true;const uint32 ID=Binding->ResourceFor(Instance.grid);
                if(M==D.Material&&ID&&ID<uint32(Catalog->Sources().Num())){const auto& Source=Catalog->Sources()[ID];vxc::AssetCandidateSourceSample Sample;
                    if(Source.Appearance&&Source.Appearance->PitchMm()==100&&Source.Sparse&&vxc::assetCandidateSourceSample(Instance,V.X,V.Y,V.Z,2,true,Sample)&&Source.Sparse->Lookup(FIntVector(Sample.x,Sample.y,Sample.z),D.Material,D.BaseRGB,D.SourceCell)){
                        D.Approved=true;D.SourceYawQuarter=Instance.yawQuarter;D.Needle=Source.Appearance->IsNeedle();D.FoliageMask=Source.Appearance->UsesFoliageMask();
                    }
                }
                break;
            }
            if(Winner)break;
        }
    }
    return true;
}
