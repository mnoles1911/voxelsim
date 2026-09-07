#include "voxelcore/amplifier.h"
#include "voxelcore/tilestore.h"
#include "voxelcore/assetgrid.h"
#include "voxelcore/assetslopefit.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <array>
#include <cstdio>
#include <cfloat>
using namespace vxc;
struct Model {double x0=DBL_MAX,y0=DBL_MAX,x1=-DBL_MAX,y1=-DBL_MAX,low=DBL_MAX,height=0;double bottom[16];int type;};
Model load(const char* name,int mm,int type){
 Model m;m.type=type; for(auto& b:m.bottom)b=DBL_MAX;
 std::ifstream f(std::string("asset-forge/out/environment-lod-prototype/")+name+"-"+std::to_string(mm)+"mm.vxa",std::ios::binary);
 std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),{});AssetGrid g;if(g.parse(data.data(),data.size())!=AssetParseError::kOk)std::exit(2);
 m.height=g.sizeZ()*mm;std::vector<std::array<double,3>> feet;
 for(int x=0;x<g.sizeX();++x)for(int y=0;y<g.sizeY();++y){bool found=false;g.columnRuns(x,y,[&](int z,int len,MaterialId mat){if(found||mat==0||(type==0&&mat!=16&&mat!=17&&mat!=18&&mat!=23))return;found=true;feet.push_back({double((x+g.originX())*mm),double((y+g.originY())*mm),double((z+g.originZ())*mm)});m.low=std::min(m.low,feet.back()[2]);});}
 double band=type==0?500.:type==1?m.height*.3:0.;
 for(auto p:feet)if(p[2]<=m.low+band){m.x0=std::min(m.x0,p[0]);m.y0=std::min(m.y0,p[1]);m.x1=std::max(m.x1,p[0]+mm);m.y1=std::max(m.y1,p[1]+mm);}
 for(auto p:feet)if(p[2]<=m.low+band){int x0=std::clamp(int(std::floor((p[0]-m.x0)*4/(m.x1-m.x0))),0,3),y0=std::clamp(int(std::floor((p[1]-m.y0)*4/(m.y1-m.y0))),0,3),x1=std::clamp(int(std::floor((p[0]+mm-m.x0)*4/(m.x1-m.x0))),0,3),y1=std::clamp(int(std::floor((p[1]+mm-m.y0)*4/(m.y1-m.y0))),0,3);for(int x=x0;x<=x1;++x)for(int y=y0;y<=y1;++y)m.bottom[x*4+y]=std::min(m.bottom[x*4+y],p[2]);}
 return m;
}
AssetSlopeFit fit(const Model& m,const Amplifier& amp,double xm,double ym){
 double h[25];for(int x=0;x<5;++x)for(int y=0;y<5;++y)h[x*5+y]=amp.column(int64_t(std::floor((xm*1000+m.x0+(m.x1-m.x0)*x/4)/100)),int64_t(std::floor((ym*1000+m.y0+(m.y1-m.y0)*y/4)/100))).surfaceMm;
 AssetSupportBin bins[16];int n=0;for(int x=0;x<4;++x)for(int y=0;y<4;++y)if(m.bottom[x*4+y]!=DBL_MAX){auto& b=bins[n++];b.bottomMm=m.bottom[x*4+y];b.groundLowMm=DBL_MAX;b.groundHighMm=-DBL_MAX;for(int dx=0;dx<2;++dx)for(int dy=0;dy<2;++dy){b.groundLowMm=std::min(b.groundLowMm,h[(x+dx)*5+y+dy]);b.groundHighMm=std::max(b.groundHighMm,h[(x+dx)*5+y+dy]);}}
 return fitAssetSupports(bins,n,h[12]-m.low,m.type<2?100.:25.,m.type==0?800.:m.type==1?m.height*.45:m.type==2?150.:50.,m.type==0?1000.:m.type==1?m.height*.65:m.type==2?300.:100.,m.type==0?25.:0.);
}
int main(){
 const char* root="tile-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4/000000000135276f/s1";
 TileGridSampler coarse(20260719,1);for(auto& p:std::filesystem::directory_iterator(root))if(p.path().extension()==".vxtl")coarse.loadTileFile(p.path());
 FineTileSampler fine(20260719,&coarse);for(int x=-5;x<=-4;++x)for(int y=-5;y<=-4;++y){auto p=std::string("tile-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4-b5e821e98/000000000135276f/s16/")+std::to_string(x)+"_"+std::to_string(y)+".vxtl";std::printf("load %s %d\n",p.c_str(),fine.loadTileFile(p));}
 Amplifier amp(20260719,fine);Model models[]={load("temperate-oak",50,0),load("granite-boulder",50,1),load("bramble-thicket",25,2),load("meadow-daisy",25,3)};
 const int offsets[9][2]={{0,0},{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}};
 for(int i=0;i<4;++i)for(int c=0;c<9;++c){auto f=fit(models[i],amp,-61422+offsets[c][0]*2.,-61440+i*15+offsets[c][1]*2.);std::printf("OLD asset%d c%d accepted%d sink%.0f burial%.0f z%.0f\n",i,c,f.accepted,f.sinkMm,f.maximumBurialMm,f.originZMm);}
 int found=0;for(int r=0;r<=12&&found<5;++r)for(int dx=-r;dx<=r&&found<5;++dx)for(int dy=-r;dy<=r&&found<5;++dy){if(std::max(std::abs(dx),std::abs(dy))!=r)continue;double sx=-61440+dx*8.,sy=-61440+dy*8.;bool all=true;AssetSlopeFit fits[4];int chosen[4];for(int i=0;i<4;++i){bool yes=false;for(int c=0;c<9;++c){auto f=fit(models[i],amp,sx+18+offsets[c][0]*2.,sy+i*15+offsets[c][1]*2.);if(f.accepted){fits[i]=f;chosen[i]=c;yes=true;break;}}all&=yes;}if(all){std::printf("FIXTURE spawn %.0f,%.0f",sx,sy);for(int i=0;i<4;++i)std::printf(" asset%d[c%d sink%.0f burial%.0f]",i,chosen[i],fits[i].sinkMm,fits[i].maximumBurialMm);std::printf("\n");++found;}}
}
