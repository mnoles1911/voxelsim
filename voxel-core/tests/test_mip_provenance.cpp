#include "voxelcore/mips.h"
#include "vxctest.h"
#include <algorithm>
#include <array>
#include <climits>
using namespace vxc;
namespace {
// Independent specification oracle: sort nonair votes instead of reproducing
// the implementation's counts array. Existing test_mips goldens also run in
// this target, pinning the pre-refactor actual brick output.
MaterialId oracleMaterial(const MaterialId (&cells)[8],int threshold,bool surface){
    std::array<MaterialId,8> sorted{};int count=0;
    for(auto m:cells)if(m!=MAT_AIR)sorted[static_cast<size_t>(count++)]=m;
    if(count<threshold||count==0)return MAT_AIR;
    if(surface){for(int z=1;z>=0;--z)for(int i=z*4;i<z*4+4;++i)if(cells[i]!=MAT_AIR)return cells[i];}
    std::sort(sorted.begin(),sorted.begin()+count);
    MaterialId winner=MAT_AIR;int best=0;
    for(int first=0;first<count;){int end=first+1;while(end<count&&sorted[static_cast<size_t>(end)]==sorted[static_cast<size_t>(first)])++end;if(end-first>best){best=end-first;winner=sorted[static_cast<size_t>(first)];}first=end;}
    return winner;
}
uint8_t oracleChild(const MaterialId (&cells)[8],MaterialId material,bool surface){
    if(material==MAT_AIR)return 255;
    if(surface){for(int z=1;z>=0;--z)for(int i=z*4;i<z*4+4;++i)if(cells[i]!=MAT_AIR)return static_cast<uint8_t>(i);}
    return static_cast<uint8_t>(std::find(std::begin(cells),std::end(cells),material)-std::begin(cells));
}
bool matches(const MaterialId (&cells)[8],int threshold,bool surface){
    const auto expected=oracleMaterial(cells,threshold,surface);const auto result=reduceMipCell(cells,threshold,surface);
    return result.material==expected&&result.childIndex==oracleChild(cells,expected,surface)&&
        (result.childIndex==255?result.material==MAT_AIR:result.childIndex<8&&cells[result.childIndex]==result.material);
}
}
VXC_TEST(mip_provenance_exhaustive_four_material_groups){
    constexpr MaterialId alphabet[4]={MAT_AIR,MAT_ROCK,MAT_GRASS,MAT_LEAF_BROADLEAF};
    uint64_t checked=0;
    for(uint32_t pattern=0;pattern<65536;++pattern){MaterialId cells[8];uint32_t code=pattern;for(int i=0;i<8;++i){cells[i]=alphabet[code&3u];code>>=2;}
        for(int threshold=-1;threshold<=9;++threshold)for(bool surface:{false,true}){if(!matches(cells,threshold,surface)){CHECK(false);return;}++checked;}}
    CHECK_EQ(checked,1441792ull);
}
VXC_TEST(mip_provenance_all_materials_threshold_extremes_and_repeated_winners){
    uint64_t state=987654321;
    for(int sample=0;sample<8192;++sample){MaterialId cells[8];for(auto& m:cells){state=splitmix64(state);m=static_cast<MaterialId>(state%static_cast<uint64_t>(kMaterialCount));}
        for(int threshold:{INT_MIN,-7,0,1,4,8,9,INT_MAX})for(bool surface:{false,true})CHECK(matches(cells,threshold,surface));}
    for(int id=1;id<kMaterialCount;++id){MaterialId cells[8];std::fill(std::begin(cells),std::end(cells),static_cast<MaterialId>(id));const auto vote=reduceMipCell(cells),top=reduceMipCell(cells,4,true);CHECK_EQ(vote.material,id);CHECK_EQ(vote.childIndex,0);CHECK_EQ(top.material,id);CHECK_EQ(top.childIndex,4);}
    // Equal material at multiple cells must select a real contributor, never
    // the geometric centre or the last matching child encountered.
    MaterialId tie[8]={MAT_LEAF_BROADLEAF,MAT_ROCK,MAT_AIR,MAT_LEAF_BROADLEAF,MAT_ROCK,MAT_AIR,MAT_AIR,MAT_AIR};
    CHECK_EQ(reduceMipCell(tie).material,MAT_ROCK);CHECK_EQ(reduceMipCell(tie).childIndex,1);
    CHECK_EQ(reduceMipCell(tie,4,true).childIndex,4);
    MaterialId air[8]={};for(int threshold:{INT_MIN,0,1,8,INT_MAX})for(bool surface:{false,true}){auto result=reduceMipCell(air,threshold,surface);CHECK_EQ(result.material,MAT_AIR);CHECK_EQ(result.childIndex,255);}
}
VXC_TEST(mip_provenance_actual_bricks_mapping_and_unchanged_materials){
    uint64_t seed=321;
    for(int sample=0;sample<24;++sample){std::array<Brick<8>,8> owned;const Brick<8>* children[8];
        for(int child=0;child<8;++child){children[child]=((sample+child)%5==0)?nullptr:&owned[static_cast<size_t>(child)];for(int z=0;z<8;++z)for(int y=0;y<8;++y)for(int x=0;x<8;++x){seed=splitmix64(seed);owned[static_cast<size_t>(child)].set(x,y,z,static_cast<MaterialId>(seed%static_cast<uint64_t>(kMaterialCount)));}}
        for(int threshold:{0,1,4,8,9})for(bool surface:{false,true}){const auto parent=downsampleBricks<8>(children,threshold,surface);
            for(int z=0;z<8;++z)for(int y=0;y<8;++y)for(int x=0;x<8;++x){
                // Derive group coordinates in a 16-cubed fine lattice first,
                // independently from the implementation's half-brick branches.
                const int fx=x*2,fy=y*2,fz=z*2,brick=(fx/8)+2*(fy/8)+4*(fz/8);MaterialId cells[8]={};
                if(children[brick])for(int i=0;i<8;++i)cells[i]=children[brick]->get(fx%8+(i%2),fy%8+((i/2)%2),fz%8+i/4);
                CHECK_EQ(parent.get(x,y,z),oracleMaterial(cells,threshold,surface));
                const auto selection=reduceMipCell(cells,threshold,surface);if(selection.childIndex!=255){const int i=selection.childIndex;CHECK(children[brick]!=nullptr);CHECK_EQ(children[brick]->get(fx%8+(i%2),fy%8+((i/2)%2),fz%8+i/4),parent.get(x,y,z));}
            }
        }
    }
}
VXC_TEST(mip_provenance_recursive_path_offsets_are_actual_leaf_contributors){
    // Trace two reduction levels through an asymmetric 4-cubed group. Each
    // winning grandchild has real original coordinates, including negative
    // world placement. No representative-material coincidence is sufficient.
    MaterialId fine[4][4][4];for(int z=0;z<4;++z)for(int y=0;y<4;++y)for(int x=0;x<4;++x)fine[z][y][x]=((x+2*y+3*z)%3==0)?MAT_LEAF_BROADLEAF:MAT_ROCK;
    for(bool surface:{false,true}){MaterialId coarse[8];MipReductionResult picks[8];
        for(int i=0;i<8;++i){MaterialId group[8];for(int j=0;j<8;++j)group[j]=fine[(i/4)*2+j/4][((i/2)%2)*2+(j/2)%2][(i%2)*2+j%2];picks[i]=reduceMipCell(group,4,surface);coarse[i]=picks[i].material;}
        const auto parent=reduceMipCell(coarse,4,surface);CHECK(parent.childIndex<8);const int i=parent.childIndex,j=picks[i].childIndex;CHECK(j<8);
        const int x=(i%2)*2+j%2,y=((i/2)%2)*2+(j/2)%2,z=(i/4)*2+j/4;CHECK_EQ(fine[z][y][x],parent.material);
        const int worldX=-12+x,worldY=-8+y,worldZ=-4+z;CHECK(worldX>=-12&&worldX<-8);CHECK(worldY>=-8&&worldY<-4);CHECK(worldZ>=-4&&worldZ<0);
        CHECK(x-2>=-2&&x-2<=1);CHECK(y-2>=-2&&y-2<=1);CHECK(z-2>=-2&&z-2<=1);
    }
}
