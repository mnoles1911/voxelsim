// Compile twice: normal include path, and with the frozen pre-change include
// directory first. No UE/GPU/filesystem fixtures needed. This isolates resolver
// math with cheap terrain facts; it does not measure full Amplifier work.
#include "voxelcore/assetfield.h"
#include <chrono>
#include <cstdio>
#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#endif
using namespace vxc;
int main(){
    struct Bank: IAssetBankSource {const AssetGrid* bankGrid(uint16_t,uint16_t)const override{return nullptr;}} bank;
    AssetLayer layer;layer.cellMm=6000;layer.maxRadiusMm=6000;
    std::vector<AssetSpecies> species(49);EcoPlacementConfig c;c.biomeMask=uint16_t(1u<<TEMPERATE_FOREST);
    for(int i=0;i<49;++i){auto& s=species[i];s.bankId=uint16_t(i);s.heightMm=12000;s.weightPerMille[TEMPERATE_FOREST]=1000;
        EcoSpeciesProfile p;p.bankId=uint16_t(i);p.stableId=i+1;p.tree=true;p.communityWeights={1000,900,1000,800};
        p.variants={{uint64_t(i+1),0,12000,true,5500,1800}};c.species.push_back(p);}
    AssetField field;field.setSeed(42);field.setLayers(&layer,1);field.setSpecies(species.data(),int(species.size()));
    field.setBankSource(&bank);
    if(!field.setEcology(c))return 2;
    for(int width:{32,320,2560,8192}){
        uint64_t digest=0,outputs=0,facts=0;
        auto sample=[&](int64_t x,int64_t y){++facts;AssetColumnFacts f;f.known=true;f.anchorSolid=true;f.slopeMmPerM=x<0?450:80;f.curv=y<0?85:175;f.heat=150;return f;};
        const int repeats=width>2560?3:20;const auto start=std::chrono::steady_clock::now();
        for(int n=0;n<repeats;++n){const int64_t x=int64_t(n%3-1)*width;const auto out=field.instancesForRect({x,-width/2,x+width-1,width/2-1},sample,true);
            outputs+=out.size();for(const auto& i:out)digest=splitmix64(digest^uint64_t(i.anchorXMm)^uint64_t(i.anchorYMm)^uint64_t(i.bankId)^uint64_t(i.seedIndex)^uint64_t(i.yawQuarter));}
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        size_t peak=0;
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS counters{};if(GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters)))peak=counters.PeakWorkingSetSize;
#endif
        std::printf("width=%d repeats=%d ms=%.3f outputs=%llu facts=%llu digest=%llu processPeakBytes=%zu\n",width,repeats,ms,(unsigned long long)outputs,(unsigned long long)facts,(unsigned long long)digest,peak);
    }
}
