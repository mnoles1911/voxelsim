#pragma once
// Initial-generation ecology. No mutable simulation, renderer state or chunk
// coordinates: callers supply habitat-qualified candidates and a complete halo.
#include "voxelcore/hash.h"
#include <algorithm>
#include <cstdint>
#include <vector>
#include <map>
#include <array>

namespace vxc {
// Increment whenever placement semantics change. Compiled world-generation
// inputs must match this version; schema compatibility alone is insufficient.
inline constexpr int kEcoAlgorithmVersion=2;
enum class EcoStand : uint8_t { Mixed, Young, Open, Ancient, Thicket };
struct EcoConfig {
    int32_t communityMm = 384000;
    int32_t standMm = 72000;
    int32_t featureCellMm = 160000;
    int32_t featureRadiusMm = 36000;
    uint16_t ancientPerMille = 240;
    uint16_t thicketPerMille = 300;
    uint16_t communityCount = 4;
};
inline bool ecoConfigValid(const EcoConfig& c) {
    return c.communityMm >= 1000 && c.communityMm <= 10000000 &&
        c.standMm >= 1000 && c.standMm <= 10000000 &&
        c.featureCellMm >= 1000 && c.featureCellMm <= 10000000 &&
        c.featureRadiusMm > 0 && c.featureRadiusMm <= c.featureCellMm / 4 &&
        c.ancientPerMille + c.thicketPerMille <= 1000 && c.communityCount > 0;
}
// Explicit supported coordinate domain keeps all distance squares bounded.
inline bool ecoCoordinateValid(int64_t x, int64_t y) {
    constexpr int64_t limit = int64_t(1) << 50;
    return x >= -limit && x <= limit && y >= -limit && y <= limit;
}
struct EcoContext {
    uint16_t community = 0;
    EcoStand stand = EcoStand::Mixed;
    uint16_t targetHeightPerMille = 650;
    uint16_t treeKeepPerMille = 750;
    uint16_t featureStrengthPerMille = 0;
};
// Host terrain channels, not inferred soil facts. Unknown baked channels are
// neutral. Slope is the amplifier's L1 rise/run in mm per metre, not degrees.
struct EcoTerrain {
    int64_t slopeMmPerM=0;
    int16_t curvature=-1,heat=-1,talus=-1;
};
inline bool ecoContextAt(uint64_t seed, int64_t x, int64_t y,
                         const EcoConfig& c, EcoContext& out,const EcoTerrain& terrain={}) {
    out = {};
    if (!ecoConfigValid(c) || !ecoCoordinateValid(x,y)) return false;
    // Jittered regional Voronoi centers, with stable tie order. Community
    // blending is left to species weights; this is the dominant community.
    const int64_t rx = floorDiv(x,int64_t(c.communityMm));
    const int64_t ry = floorDiv(y,int64_t(c.communityMm));
    int64_t best = INT64_MAX;
    for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
        const auto h=hash2(seed,rx+dx,ry+dy,CH_ECO_COMMUNITY);
        const int64_t cx=(rx+dx)*c.communityMm+c.communityMm/4+
            int64_t(h%uint64_t(c.communityMm/2));
        const int64_t cy=(ry+dy)*c.communityMm+c.communityMm/4+
            int64_t((h>>32)%uint64_t(c.communityMm/2));
        const int64_t d=(x-cx)*(x-cx)+(y-cy)*(y-cy);
        if(d<best){best=d;out.community=uint16_t(splitmix64(h)%c.communityCount);}
    }
    // Authored initial-generation response: exposed convex/steep ground is
    // more open; concave, sheltered ground supports taller stands. This is
    // an artistic response to measured terrain, not an ecological simulation.
    const int64_t slope=std::clamp<int64_t>(terrain.slopeMmPerM,0,2000);
    const int curv=terrain.curvature<0?0:std::clamp<int>(terrain.curvature,0,254)-128;
    const int heat=terrain.heat<0?0:std::max(0,std::min<int>(terrain.heat,254)-127);
    const int talus=std::clamp<int>(terrain.talus,0,254);
    const int64_t exposure=std::max<int64_t>(0,slope-150)*12+
        std::max(0,-curv)*45+heat*20+talus*25;
    const int64_t structure=valueNoise2(seed,x,y,c.standMm,CH_ECO_STRUCTURE)-exposure;
    if(structure < -12000){out.stand=EcoStand::Open;out.treeKeepPerMille=180;out.targetHeightPerMille=600;}
    else if(structure > 10000){out.stand=EcoStand::Young;out.treeKeepPerMille=900;out.targetHeightPerMille=300;}
    // Feature centers remain in the middle half of their cell. Radius <=
    // cell/4 guarantees disjoint feature interiors and bounds queries to 3x3.
    const int64_t fx=floorDiv(x,int64_t(c.featureCellMm));
    const int64_t fy=floorDiv(y,int64_t(c.featureCellMm));
    for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
        const auto h=hash2(seed,fx+dx,fy+dy,CH_ECO_FEATURE);
        const uint32_t draw=uint32_t(splitmix64(h)%1000);
        if(draw>=uint32_t(c.ancientPerMille)+c.thicketPerMille)continue;
        const int64_t cx=(fx+dx)*c.featureCellMm+c.featureCellMm/4+int64_t(h%uint64_t(c.featureCellMm/2));
        const int64_t cy=(fy+dy)*c.featureCellMm+c.featureCellMm/4+int64_t((h>>32)%uint64_t(c.featureCellMm/2));
        const int64_t d=(x-cx)*(x-cx)+(y-cy)*(y-cy);
        const int64_t radius2=int64_t(c.featureRadiusMm)*c.featureRadiusMm;
        if(d>=radius2)continue;
        // A bounded, warped footprint: coherent lobes and seeded anisotropy
        // replace identical discs. Normalize before squaring to avoid overflow.
        const int64_t u=(x-cx)*1000/c.featureRadiusMm;
        const int64_t v=(y-cy)*1000/c.featureRadiusMm;
        const auto shape=splitmix64(h^0x6d5a31b7ULL);
        const int64_t shear=int64_t(shape%501)-250;
        const int64_t a=u+v*shear/1000;
        const int64_t b=v+u*shear/1000;
        const int64_t stretch=1000+int64_t((shape>>12)%501);
        const int64_t metric=(shape&1)?a*a*stretch/1000+b*b:a*a+b*b*stretch/1000;
        const int64_t noise=valueNoise2Fade(seed^shape,x,y,
            std::max<int32_t>(1,c.featureRadiusMm/2),CH_ECO_FEATURE);
        const bool ancient=draw<c.ancientPerMille;
        // Suitable terrain clips the footprint itself, so patches follow
        // terrain instead of only losing individual trees after selection.
        const int64_t suitability=std::clamp<int64_t>(1000-
            std::max<int64_t>(0,slope-(ancient?200:450))*(ancient?2:1)-
            talus*2-std::max(0,-curv)*(ancient?3:1),0,1000);
        const int64_t threshold=(680000+int64_t((shape>>24)%160001)+noise*7)*suitability/1000;
        // Fade both the irregular edge and the strict outer envelope. The
        // latter preserves disjoint influence and the existing query bound.
        const int64_t strength=std::min<int64_t>((threshold-metric)*1000/300000,
            (radius2-d)*1000/(std::max<int64_t>(1,radius2/3)));
        if(strength<=0)continue;
        out.featureStrengthPerMille=uint16_t(std::min<int64_t>(1000,strength));
        const auto blend=[&](int base,int target){return uint16_t(base+
            (target-base)*int(out.featureStrengthPerMille)/1000);};
        out.targetHeightPerMille=blend(out.targetHeightPerMille,ancient?950:250);
        out.treeKeepPerMille=blend(out.treeKeepPerMille,ancient?650:400);
        out.stand=ancient?EcoStand::Ancient:EcoStand::Thicket;
    }
    const int64_t vigor=std::clamp<int64_t>(1000+curv-slope/4-heat-talus,500,1100);
    out.targetHeightPerMille=uint16_t(std::min<int64_t>(1000,out.targetHeightPerMille*vigor/1000));
    out.treeKeepPerMille=uint16_t(out.treeKeepPerMille*std::clamp<int64_t>(1000-exposure/40,350,1000)/1000);
    return true;
}

struct EcoTree {
    uint64_t id=0; // Stable placement ID, not a vector index or variant seed.
    int64_t xMm=0,yMm=0;
    int32_t exclusionMm=1000,crownMm=4000;
    uint16_t opacityPerMille=700;
};
inline bool ecoTreeValid(const EcoTree& t){
    return ecoCoordinateValid(t.xMm,t.yMm)&&t.exclusionMm>0&&
        t.exclusionMm<=100000&&t.crownMm>0&&t.crownMm<=100000&&t.opacityPerMille<=1000;
}
// Local priority thinning compares against ALL eligible candidates, including
// rejected ones. No recursive winner chains: partition independence requires
// only a halo of twice the largest exclusion radius. Some density is sacrificed.
inline bool ecoTreeSurvives(uint64_t seed,const EcoTree& t,const std::vector<EcoTree>& neighbors){
    if(!ecoTreeValid(t))return false;
    const auto priority=hash2(seed,int64_t(t.id>>32),int64_t(t.id&UINT32_MAX),CH_ECO_PRIORITY);
    for(const auto& n:neighbors){
        if(n.id==t.id)continue;
        if(!ecoTreeValid(n))return false;
        const int64_t r=int64_t(t.exclusionMm)+n.exclusionMm;
        const int64_t dx=t.xMm-n.xMm,dy=t.yMm-n.yMm;
        if(dx<=-r||dx>=r||dy<=-r||dy>=r||dx*dx+dy*dy>=r*r)continue;
        const auto p=hash2(seed,int64_t(n.id>>32),int64_t(n.id&UINT32_MAX),CH_ECO_PRIORITY);
        if(p<priority||(p==priority&&n.id<t.id))return false;
    }
    return true;
}
// Additive optical coverage proxy makes accumulation independent of ordering.
// Use accepted trees only and a halo of the maximum crown radius.
inline uint16_t ecoCanopyAt(int64_t x,int64_t y,const std::vector<EcoTree>& trees){
    if(!ecoCoordinateValid(x,y))return 0;
    uint64_t cover=0;
    for(const auto& t:trees){
        if(!ecoTreeValid(t))continue;
        const int64_t dx=x-t.xMm,dy=y-t.yMm,r=t.crownMm;
        if(dx<=-r||dx>=r||dy<=-r||dy>=r)continue;
        const int64_t d=dx*dx+dy*dy,r2=r*r;
        if(d<r2)cover+=uint64_t((r2-d)*t.opacityPerMille/r2);
        if(cover>=1000)return 1000;
    }
    return uint16_t(cover);
}
// Immutable query-local spatial index. Buckets own records, so copying an
// index never leaves pointers into a temporary candidate vector. The source
// vector helpers above remain an independent exhaustive reference for tests.
class EcoTreeIndex {
    struct Record { EcoTree tree; uint64_t priority; };
    std::map<std::pair<int64_t,int64_t>,std::vector<Record>> buckets_;
    uint64_t seed_=0;
    int64_t pitch_=1000,maxExclusion_=0,maxCrown_=0;
    bool valid_=true;
    template<class Visit> bool visit(int64_t x,int64_t y,int64_t radius,Visit&& visitor,
                                     uint64_t* checked) const {
        const auto x0=floorDiv(x-radius,pitch_),x1=floorDiv(x+radius,pitch_);
        const auto y0=floorDiv(y-radius,pitch_),y1=floorDiv(y+radius,pitch_);
        for(auto by=y0;by<=y1;++by)for(auto bx=x0;bx<=x1;++bx){
            const auto it=buckets_.find({bx,by});if(it==buckets_.end())continue;
            for(const auto& r:it->second){if(checked)++*checked;if(!visitor(r))return false;}
        }
        return true;
    }
public:
    EcoTreeIndex(uint64_t seed,const std::vector<EcoTree>& trees):seed_(seed){
        std::map<uint64_t,bool> ids;
        for(const auto& t:trees){
            if(!ecoTreeValid(t)||!ids.emplace(t.id,true).second){valid_=false;return;}
            maxExclusion_=std::max<int64_t>(maxExclusion_,t.exclusionMm);
            maxCrown_=std::max<int64_t>(maxCrown_,t.crownMm);
        }
        pitch_=std::max<int64_t>(1000,std::max(2*maxExclusion_,maxCrown_));
        for(const auto& t:trees)buckets_[{floorDiv(t.xMm,pitch_),floorDiv(t.yMm,pitch_)}].push_back(
            {t,hash2(seed,int64_t(t.id>>32),int64_t(t.id&UINT32_MAX),CH_ECO_PRIORITY)});
    }
    bool valid()const{return valid_;}
    bool survives(const EcoTree& t,uint64_t* checked=nullptr)const{
        if(!valid_||!ecoTreeValid(t))return false;
        const auto priority=hash2(seed_,int64_t(t.id>>32),int64_t(t.id&UINT32_MAX),CH_ECO_PRIORITY);
        return visit(t.xMm,t.yMm,t.exclusionMm+maxExclusion_,[&](const Record& r){
            const auto& n=r.tree;if(n.id==t.id)return true;
            const int64_t reach=int64_t(t.exclusionMm)+n.exclusionMm;
            const int64_t dx=t.xMm-n.xMm,dy=t.yMm-n.yMm;
            if(dx<=-reach||dx>=reach||dy<=-reach||dy>=reach||dx*dx+dy*dy>=reach*reach)return true;
            return !(r.priority<priority||(r.priority==priority&&n.id<t.id));
        },checked);
    }
    uint16_t canopyAt(int64_t x,int64_t y,uint64_t* checked=nullptr)const{
        if(!valid_||!ecoCoordinateValid(x,y))return 0;
        uint64_t cover=0;
        visit(x,y,maxCrown_,[&](const Record& record){
            const auto& t=record.tree;const int64_t dx=x-t.xMm,dy=y-t.yMm,r=t.crownMm;
            if(dx<=-r||dx>=r||dy<=-r||dy>=r)return true;
            const int64_t d=dx*dx+dy*dy,r2=r*r;
            if(d<r2)cover+=uint64_t((r2-d)*t.opacityPerMille/r2);
            return cover<1000;
        },checked);
        return uint16_t(std::min<uint64_t>(cover,1000));
    }
};
enum class EcoCoverRole : uint8_t { Sun, Shade, SpringWoodland, Shrub, Wetland, ShadeShrub, Inert };
// Water-distance response supplements (never replaces) the host's mandatory
// depth/distance gates and its TWI moisture weighting. Values <=1000 keep the
// authored species weight bounded. Unknown distance is neutral for general
// vegetation and refuses an explicitly water-bound species.
inline uint16_t ecoHydrologyWeight(int32_t distanceMm,int32_t waterMaxMm,int moistureAffinity){
    if(distanceMm<0||distanceMm==INT32_MAX)return waterMaxMm>0?0:1000;
    if(waterMaxMm>0){
        if(distanceMm>waterMaxMm)return 0;
        return uint16_t(1000-int64_t(distanceMm)*800/waterMaxMm);
    }
    // Broad, mild bank influence for general vegetation. TWI still models
    // moisture away from mapped water and species depth tolerances still veto.
    const int near=1000-int(std::min<int64_t>(distanceMm,80000)*1000/80000);
    const int affinity=std::clamp(moistureAffinity,-2,2);
    if(affinity>0)return uint16_t(1000-(1000-near)*affinity/8);
    if(affinity<0)return uint16_t(1000+near*affinity/8);
    return 1000;
}
inline uint16_t ecoCoverWeight(EcoCoverRole role,uint16_t canopy,const EcoContext& c){
    canopy=std::min<uint16_t>(canopy,1000);
    switch(role){
        case EcoCoverRole::Sun:return uint16_t(1000-canopy);
        case EcoCoverRole::Shade:return uint16_t(150+canopy*85/100);
        case EcoCoverRole::SpringWoodland:return uint16_t(450+canopy*55/100);
        case EcoCoverRole::Shrub:{
            const int base=(1000-canopy)/4;
            // Sun-loving thickets lose vigor beneath closed crowns; feather
            // into ordinary ground cover at the site's irregular boundary.
            const int target=1000-canopy*3/4;
            return uint16_t(base+(c.stand==EcoStand::Thicket?
                (target-base)*c.featureStrengthPerMille/1000:0));}
        case EcoCoverRole::Wetland:return 1000; // Host MUST apply water/depth gates.
        case EcoCoverRole::ShadeShrub:{
            const int base=100+canopy/5;
            const int target=450+canopy/2;
            return uint16_t(base+(c.stand==EcoStand::Thicket?
                (target-base)*c.featureStrengthPerMille/1000:0));}
        case EcoCoverRole::Inert:return 1000; // Rocks respond to substrate/slope, not canopy.
    }
    return 0;
}
enum class EcoGrowthForm : uint8_t { Unspecified, Open, Woodland, Edge, Compact, Spreading, Leaning };
struct EcoVariant {
    uint64_t stableId=0;
    uint16_t seedIndex=0;
    int32_t heightMm=0;
    bool published=false;
    int32_t crownMm=4000;
    int32_t exclusionMm=1000;
    EcoGrowthForm growthForm=EcoGrowthForm::Unspecified;
};
// Prefer heights near the stand target while allowing distinct shapes. Stable
// hash races make selection independent of catalog ordering. No voxel scaling.
inline const EcoVariant* ecoChooseVariant(uint64_t seed,uint64_t placementId,
    int32_t targetHeightMm,const std::vector<EcoVariant>& variants,EcoStand stand=EcoStand::Mixed){
    if(targetHeightMm<=0)return nullptr;
    const EcoVariant* best=nullptr;uint64_t bestScore=UINT64_MAX;
    for(const auto& v:variants){
        if(!v.published||v.heightMm<=0)continue;
        const int64_t delta=int64_t(v.heightMm)-targetHeightMm;
        const uint64_t error=uint64_t(delta<0?-delta:delta)*1000/uint32_t(targetHeightMm);
        const uint64_t jitter=hash2(seed^splitmix64(placementId),int64_t(v.stableId>>32),
            int64_t(v.stableId&UINT32_MAX),CH_ECO_VARIANT)%251;
        uint64_t formPenalty=0;
        if(v.growthForm!=EcoGrowthForm::Unspecified){
            const bool woody=v.growthForm==EcoGrowthForm::Open||v.growthForm==EcoGrowthForm::Woodland||v.growthForm==EcoGrowthForm::Edge;
            const auto preferred=woody?(stand==EcoStand::Open?EcoGrowthForm::Open:
                stand==EcoStand::Thicket?EcoGrowthForm::Edge:EcoGrowthForm::Woodland):
                (stand==EcoStand::Open||stand==EcoStand::Thicket?EcoGrowthForm::Spreading:
                 stand==EcoStand::Young?EcoGrowthForm::Leaning:EcoGrowthForm::Compact);
            formPenalty=v.growthForm==preferred?0:180;
        }
        const uint64_t score=error+jitter+formPenalty;
        if(!best||score<bestScore||(score==bestScore&&v.stableId<best->stableId)){best=&v;bestScore=score;}
    }
    return best;
}

// Host compiles named library species to bank IDs once. The IDs below are
// immutable species identities; bank IDs are only lookup handles.
struct EcoSpeciesProfile {
    uint16_t bankId=0;
    uint64_t stableId=0;
    bool tree=false;
    EcoCoverRole coverRole=EcoCoverRole::Sun;
    std::vector<uint16_t> communityWeights;
    std::vector<EcoVariant> variants;
    uint16_t crownOpacityPerMille=700;
    bool ancientOnly=false;
    int32_t densitySpacingMm=0; // zero preserves legacy authored density
    uint16_t densityAbundanceQ10=1024;
    std::array<uint16_t,5> standWeights{{1000,1000,1000,1000,1000}};
};
struct EcoPlacementConfig {
    EcoConfig fields;
    uint16_t biomeMask=0;
    std::vector<EcoSpeciesProfile> species;
    const EcoSpeciesProfile* profile(uint16_t bank) const {
        for(const auto& p:species)if(p.bankId==bank)return &p;
        return nullptr;
    }
    bool valid() const {
        if(!ecoConfigValid(fields)||biomeMask==0||species.empty())return false;
        for(size_t i=0;i<species.size();++i){
            const auto& p=species[i];
            for(auto weight:p.standWeights)if(weight>1000)return false;
            if(p.densitySpacingMm<0||p.densitySpacingMm>1000000||p.densityAbundanceQ10>1024||
               (p.ancientOnly&&!p.tree))return false;
            if(p.communityWeights.size()!=fields.communityCount||p.variants.empty()||p.crownOpacityPerMille>1000)return false;
            for(auto w:p.communityWeights)if(w>1000)return false;
            for(size_t j=0;j<i;++j)if(species[j].bankId==p.bankId||species[j].stableId==p.stableId)return false;
            for(size_t j=0;j<p.variants.size();++j){
                const auto& v=p.variants[j];
                if(uint8_t(v.growthForm)>uint8_t(EcoGrowthForm::Leaning))return false;
                if(v.heightMm<=0||v.heightMm>1000000||v.crownMm<=0||v.crownMm>100000||
                   v.exclusionMm<=0||v.exclusionMm>100000)return false;
                for(size_t k=0;k<j;++k)if(p.variants[k].stableId==v.stableId||p.variants[k].seedIndex==v.seedIndex)return false;
            }
        }
        return true;
    }
};
} // namespace vxc
