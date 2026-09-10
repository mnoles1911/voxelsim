#include "voxelcore/assetecology.h"
#include "voxelcore/assetfield.h"
#include "voxelcore/hash_channel_registry.h"
#include "vxctest.h"
#include <set>
#include <future>
#include <tuple>
using namespace vxc;
VXC_TEST(ecology_hydrology_concentrates_water_bound_species_and_preserves_unknowns){
    CHECK_EQ(ecoHydrologyWeight(0,4000,2),1000);
    CHECK(ecoHydrologyWeight(1000,4000,2)>ecoHydrologyWeight(3000,4000,2));
    CHECK_EQ(ecoHydrologyWeight(4001,4000,2),0);
    CHECK_EQ(ecoHydrologyWeight(INT32_MAX,4000,2),0);
    CHECK_EQ(ecoHydrologyWeight(INT32_MAX,0,2),1000);
    CHECK(ecoHydrologyWeight(0,0,2)>ecoHydrologyWeight(80000,0,2));
    CHECK(ecoHydrologyWeight(0,0,-2)<ecoHydrologyWeight(80000,0,-2));
    CHECK_EQ(ecoHydrologyWeight(0,0,0),ecoHydrologyWeight(80000,0,0));
}
VXC_TEST(ecology_tree_and_detail_outputs_match_full_region_in_parallel){
    struct Bank: IAssetBankSource{const AssetGrid* bankGrid(uint16_t,uint16_t)const override{return nullptr;}}bank;
    AssetLayer layers[2];layers[0].cellMm=6000;layers[0].maxRadiusMm=6000;
    layers[1].cellMm=2200;layers[1].maxRadiusMm=1000;layers[1].maxHeightMm=2000;layers[1].terrainLattice=false;
    AssetSpecies species[3];
    for(int i=0;i<3;++i){species[i].bankId=uint16_t(i);species[i].layer=uint8_t(i?1:0);
        species[i].heightMm=i?500:12000;species[i].voxelSizeMm=i?25:100;
        species[i].weightPerMille[TEMPERATE_FOREST]=1000;}
    AssetField field;field.setSeed(55);field.setLayers(layers,2);field.setSpecies(species,3);field.setBankSource(&bank);
    EcoPlacementConfig config;config.biomeMask=uint16_t(1u<<TEMPERATE_FOREST);
    for(int i=0;i<3;++i){EcoSpeciesProfile p;p.bankId=uint16_t(i);p.stableId=uint64_t(20-i);p.tree=i==0;
        p.coverRole=i==1?EcoCoverRole::Sun:EcoCoverRole::Shade;p.communityWeights={1000,900,1000,800};
        p.variants={{uint64_t(i+10),uint16_t(i+7),i?500:12000,true,i?700:5500,i?200:1800}};
        config.species.push_back(p);}
    CHECK(field.setEcology(config));
    // A cache-invalidating copy must retain ecological placement and its wider read halo.
    AssetField copied(field);
    CHECK(copied.ecologyEnabled());
    CHECK_EQ(copied.columnSamplingReachMm(),field.columnSamplingReachMm());
    AssetField assigned;assigned.setSeed(123);
    const auto assignmentRevision=assigned.configurationRevision();
    assigned=field;
    CHECK(assigned.configurationRevision()>assignmentRevision);
    CHECK(assigned.ecologyEnabled());
    CHECK_EQ(assigned.seed(),field.seed());
    CHECK_EQ(assigned.columnSamplingReachMm(),field.columnSamplingReachMm());
    const auto ecologyRevision=assigned.configurationRevision();
    CHECK(assigned.setEcology(config));
    CHECK(assigned.configurationRevision()>ecologyRevision);
    EcoPlacementConfig invalid=config;invalid.biomeMask=0;
    const auto failedRevision=assigned.configurationRevision();
    CHECK(!assigned.setEcology(invalid));
    CHECK(!assigned.ecologyEnabled());
    CHECK(assigned.configurationRevision()>failedRevision);
    const auto samplingReach=field.columnSamplingReachMm();
    CHECK(samplingReach>layers[0].maxRadiusMm);
    bool sampledOutsidePhysicalReach=false;
    std::set<std::pair<int64_t,int64_t>> sampledCoordinates;
    field.instancesForRect({-10,-10,9,9},[&](int64_t x,int64_t y){
        CHECK(sampledCoordinates.insert({x,y}).second);
        CHECK(x*100>=-1000-samplingReach);CHECK(x*100<=999+samplingReach);
        CHECK(y*100>=-1000-samplingReach);CHECK(y*100<=999+samplingReach);
        if(x*100<-1000-layers[0].maxRadiusMm||x*100>999+layers[0].maxRadiusMm)sampledOutsidePhysicalReach=true;
        AssetColumnFacts f;f.known=true;f.anchorSolid=true;return f;
    });
    CHECK(sampledOutsidePhysicalReach);
    int nextQueryCalls=0;
    const auto unavailable=field.instancesForRect({-10,-10,9,9},[&](int64_t,int64_t){
        ++nextQueryCalls;return AssetColumnFacts{};
    });
    CHECK(nextQueryCalls>0);CHECK(unavailable.empty());
    auto facts=[](int64_t x,int64_t y){AssetColumnFacts f;f.known=true;f.anchorSolid=true;
        f.slopeMmPerM=x<0?450:80;f.curv=y<0?85:175;f.heat=150;
        if(x>300&&y>300)f.standingWaterMm=10000;return f;};
    const auto whole=field.instancesForRect({-600,-600,599,599},facts);
    const auto copiedWhole=copied.instancesForRect({-600,-600,599,599},facts);
    CHECK_EQ(copiedWhole.size(),whole.size());
    for(size_t i=0;i<whole.size();++i){
        CHECK_EQ(copiedWhole[i].anchorXMm,whole[i].anchorXMm);
        CHECK_EQ(copiedWhole[i].anchorYMm,whole[i].anchorYMm);
        CHECK_EQ(copiedWhole[i].bankId,whole[i].bankId);
        CHECK_EQ(copiedWhole[i].seedIndex,whole[i].seedIndex);
    }
    using Key=std::tuple<int64_t,int64_t,uint16_t,uint16_t,uint8_t>;
    auto key=[](const AssetInstance& i){return Key{i.anchorXMm,i.anchorYMm,i.bankId,i.seedIndex,i.layer};};
    // Owned-anchor streaming must reproduce the old overlapping query plus
    // host filter exactly, including ordering, chosen seed, yaw and anchor Z.
    for(int mode=0;mode<3;++mode)for(uint64_t seed:{0ull,42ull,901ull}){
        AssetField ownedField;ownedField.setSeed(seed);ownedField.setLayers(layers,2);
        ownedField.setSpecies(species,3);ownedField.setBankSource(&bank);
        if(mode!=2){auto c=config;if(mode==1)c.biomeMask=uint16_t(1u<<((TEMPERATE_FOREST+1)%kBiomeCount));CHECK(ownedField.setEcology(c));}
        auto checkOwned=[&](AssetVoxelRect rect){
            const auto full=ownedField.instancesForRect(rect,facts);
            // Full/detail queries bypass raw tree reuse. They independently
            // exercise the original two-pass resolver, including inactive
            // biome fallback, while this terrain-only query uses the memo.
            std::vector<AssetInstance> referenceTrees;
            for(const auto& i:full)if(layers[i.layer].terrainLattice)referenceTrees.push_back(i);
            const auto reusedTrees=ownedField.instancesForRect(rect,facts,true);
            CHECK_EQ(reusedTrees.size(),referenceTrees.size());
            for(size_t j=0;j<std::min(reusedTrees.size(),referenceTrees.size());++j){
                CHECK(key(reusedTrees[j])==key(referenceTrees[j]));
                CHECK_EQ(reusedTrees[j].anchorVz,referenceTrees[j].anchorVz);
                CHECK_EQ(reusedTrees[j].yawQuarter,referenceTrees[j].yawQuarter);
                CHECK_EQ(reusedTrees[j].speciesIndex,referenceTrees[j].speciesIndex);
            }
            std::vector<AssetInstance> expected;
            for(const auto& i:full){const auto x=floorDiv(i.anchorXMm,int64_t(kVoxelSizeMm)),y=floorDiv(i.anchorYMm,int64_t(kVoxelSizeMm));
                if(!layers[i.layer].terrainLattice&&x>=rect.vx0&&x<=rect.vx1&&y>=rect.vy0&&y<=rect.vy1)expected.push_back(i);}
            const auto actual=ownedField.detailInstancesOwnedByRect(rect,facts);
            CHECK_EQ(actual.size(),expected.size());
            for(size_t j=0;j<std::min(actual.size(),expected.size());++j){
                CHECK(key(actual[j])==key(expected[j]));CHECK_EQ(actual[j].anchorVz,expected[j].anchorVz);
                CHECK_EQ(actual[j].yawQuarter,expected[j].yawQuarter);CHECK_EQ(actual[j].speciesIndex,expected[j].speciesIndex);}
        };
        for(const auto rect:{AssetVoxelRect{-100,-100,-1,-1},AssetVoxelRect{-32,-32,31,31},
            AssetVoxelRect{0,0,63,63},AssetVoxelRect{64,-64,127,-1},AssetVoxelRect{1,1,0,0}})checkOwned(rect);
        const auto anchors=ownedField.detailInstancesOwnedByRect({-100,-100,99,99},facts);
        CHECK(!anchors.empty());
        const auto x=floorDiv(anchors.front().anchorXMm,int64_t(kVoxelSizeMm));
        const auto y=floorDiv(anchors.front().anchorYMm,int64_t(kVoxelSizeMm));
        checkOwned({x,y,x,y});checkOwned({x-1,y,x-1,y});checkOwned({x+1,y,x+1,y});
    }
    const auto decisions=field.ecologicalDecisionsForRect({-600,-600,599,599},facts);
    // Exceed the 4096-entry optimization cap. Diagnostic terrain queries
    // bypass reuse, so the uncached tail must retain identical order/results.
    {
        const AssetVoxelRect large{-2500,-2500,2499,2499};
        const auto actual=field.instancesForRect(large,facts,true);
        const auto traced=field.ecologicalDecisionsForRect(large,facts,true);
        std::vector<AssetInstance> expected;
        for(const auto& d:traced)if(d.reason==EcoDecisionReason::Accepted||d.reason==EcoDecisionReason::LegacyAccepted)expected.push_back(d.instance);
        CHECK_EQ(actual.size(),expected.size());
        for(size_t j=0;j<std::min(actual.size(),expected.size());++j){
            CHECK(key(actual[j])==key(expected[j]));CHECK_EQ(actual[j].anchorVz,expected[j].anchorVz);
            CHECK_EQ(actual[j].yawQuarter,expected[j].yawQuarter);CHECK_EQ(actual[j].speciesIndex,expected[j].speciesIndex);
        }
    }
    std::set<Key> explained,normal;
    for(const auto& i:whole)normal.insert(key(i));
    size_t filtered=0;
    for(const auto& d:decisions){
        CHECK(d.facts.known);CHECK(d.canopyPerMille<=1000);
        if(d.reason==EcoDecisionReason::Accepted){
            CHECK(d.hasContext);CHECK(d.hasInstance);
            CHECK(d.targetHeightMm>0);explained.insert(key(d.instance));
        }else ++filtered;
    }
    CHECK(explained==normal);CHECK(filtered>0);
    const auto unknownDecisions=field.ecologicalDecisionsForRect({-10,-10,9,9},
        [](int64_t,int64_t){return AssetColumnFacts{};});
    CHECK(!unknownDecisions.empty());
    for(const auto& d:unknownDecisions){
        CHECK(d.reason==EcoDecisionReason::UnknownFacts);CHECK(!d.hasContext);CHECK(!d.hasInstance);
    }
    auto checkPartition=[&](int quadrant){
        const int x=quadrant%2?0:-600,y=quadrant/2?0:-600;
        const auto part=field.instancesForRect({x,y,x+599,y+599},facts);
        std::set<Key> expected,actual;
        for(const auto& i:whole){const auto r=layers[i.layer].maxRadiusMm;
            if(i.anchorXMm+r>=int64_t(x)*100&&i.anchorXMm-r<int64_t(x+600)*100&&
               i.anchorYMm+r>=int64_t(y)*100&&i.anchorYMm-r<int64_t(y+600)*100)expected.insert(key(i));}
        for(const auto& i:part)actual.insert(key(i));
        return actual==expected;
    };
    std::vector<std::future<bool>> jobs;
    for(int i=3;i>=0;--i)jobs.push_back(std::async(std::launch::async,checkPartition,i));
    for(auto& job:jobs)CHECK(job.get());
    std::set<Key> treeOnly,expectedTrees;
    for(const auto& i:whole)if(i.layer==0)expectedTrees.insert(key(i));
    for(const auto& i:field.instancesForRect({-600,-600,599,599},facts,true))treeOnly.insert(key(i));
    CHECK(treeOnly==expectedTrees);CHECK(!expectedTrees.empty());CHECK(whole.size()>expectedTrees.size());
    std::reverse(species,species+3);field.setSpecies(species,3);CHECK(field.setEcology(config));
    std::set<Key> reordered,original;
    for(const auto& i:whole)original.insert(key(i));
    for(const auto& i:field.instancesForRect({-600,-600,599,599},facts))reordered.insert(key(i));
    CHECK(original==reordered);
}
VXC_TEST(ecology_shared_field_resolves_consistently_across_query_sizes){
    struct Bank: IAssetBankSource {const AssetGrid* bankGrid(uint16_t,uint16_t)const override{return nullptr;}} bank;
    AssetLayer layer;layer.cellMm=5000;layer.maxRadiusMm=5000;
    AssetSpecies species;species.bankId=0;species.heightMm=10000;species.voxelSizeMm=100;
    species.weightPerMille[TEMPERATE_FOREST]=1000;
    AssetField field;field.setSeed(42);field.setLayers(&layer,1);field.setSpecies(&species,1);field.setBankSource(&bank);
    EcoPlacementConfig config;config.biomeMask=uint16_t(1u<<TEMPERATE_FOREST);
    EcoSpeciesProfile profile;profile.tree=true;profile.stableId=55;profile.communityWeights={1000,1000,1000,1000};
    profile.variants={{1,7,3000,true,2000,1300},{2,9,10000,true,4000,1800}};
    config.species.push_back(profile);CHECK(field.setEcology(config));
    auto facts=[](int64_t,int64_t){AssetColumnFacts f;f.known=true;f.anchorSolid=true;return f;};
    const auto whole=field.instancesForRect({-500,-500,500,500},facts,true);
    CHECK(!whole.empty());
    for(int y=-400;y<400;y+=100)for(int x=-400;x<400;x+=100){
        const auto part=field.instancesForRect({x,y,x+99,y+99},facts,true);
        size_t expected=0;
        for(const auto& w:whole)if(w.anchorXMm+layer.maxRadiusMm>=int64_t(x)*100 &&
            w.anchorXMm-layer.maxRadiusMm<=int64_t(x+100)*100-1 &&
            w.anchorYMm+layer.maxRadiusMm>=int64_t(y)*100 &&
            w.anchorYMm-layer.maxRadiusMm<=int64_t(y+100)*100-1)++expected;
        CHECK_EQ(part.size(),expected);
        for(const auto& p:part){
            bool found=false;for(const auto& w:whole)if(w.anchorXMm==p.anchorXMm&&w.anchorYMm==p.anchorYMm){
                CHECK_EQ(w.seedIndex,p.seedIndex);found=true;break;
            }CHECK(found);
        }
    }
    auto submerged=[](int64_t,int64_t){AssetColumnFacts f;f.known=true;f.anchorSolid=true;f.standingWaterMm=10000;return f;};
    CHECK(field.instancesForRect({-500,-500,500,500},submerged,true).empty());
    config.species[0].ancientOnly=true;CHECK(field.setEcology(config));
    // Survey several feature cells; one small crop may legitimately contain
    // no ancient trees after the irregular footprint and spacing filters.
    const auto ancient=field.instancesForRect({-4000,-4000,3999,3999},facts,true);
    CHECK(!ancient.empty());
    for(const auto& instance:ancient){EcoContext context;
        CHECK(ecoContextAt(42,instance.anchorXMm,instance.anchorYMm,config.fields,context));
        CHECK(context.stand==EcoStand::Ancient);
    }
    config.species[0].ancientOnly=false;
    config.species[0].standWeights={{0,1000,0,0,0}};CHECK(field.setEcology(config));
    const auto young=field.instancesForRect({-2000,-2000,1999,1999},facts,true);CHECK(!young.empty());
    for(const auto& instance:young){EcoContext context;
        CHECK(ecoContextAt(42,instance.anchorXMm,instance.anchorYMm,config.fields,context));
        CHECK(context.stand==EcoStand::Young);
    }
    config.species[0].variants[0].heightMm=1000000;CHECK(!field.setEcology(config));CHECK(!field.ecologyEnabled());
}
VXC_TEST(ecology_index_matches_exhaustive_queries_and_reduces_candidate_work){
    std::vector<EcoTree> trees;
    for(int y=-32;y<32;++y)for(int x=-32;x<32;++x){
        const auto h=hash2(123,x,y,CH_ECO_COVER);
        trees.push_back({uint64_t(trees.size()+1),int64_t(x)*3000+int64_t(h%2000),
            int64_t(y)*3000+int64_t((h>>16)%2000),1000+int32_t(h%1500),
            3000+int32_t(h%4500),uint16_t(400+h%500)});
    }
    EcoTreeIndex index(42,trees);CHECK(index.valid());uint64_t comparisons=0;
    std::vector<EcoTree> accepted;
    for(const auto& t:trees){
        const bool expected=ecoTreeSurvives(42,t,trees);
        CHECK_EQ(index.survives(t,&comparisons),expected);
        if(expected)accepted.push_back(t);
    }
    CHECK(comparisons<uint64_t(trees.size())*trees.size()/20);
    EcoTreeIndex canopy(42,accepted);uint64_t coverChecks=0;
    for(int y=-100000;y<100000;y+=3700)for(int x=-100000;x<100000;x+=4100)
        CHECK_EQ(canopy.canopyAt(x,y,&coverChecks),ecoCanopyAt(x,y,accepted));
    std::printf("    spatial thinning checked %llu records for %zu candidates (exhaustive upper bound %llu)\n",
        static_cast<unsigned long long>(comparisons),trees.size(),
        static_cast<unsigned long long>(uint64_t(trees.size())*trees.size()));
    std::reverse(trees.begin(),trees.end());EcoTreeIndex reordered(42,trees);
    for(const auto& t:trees)CHECK_EQ(reordered.survives(t),index.survives(t));
    trees.push_back(trees.front());EcoTreeIndex duplicate(42,trees);CHECK(!duplicate.valid());
    EcoTreeIndex empty(42,{});CHECK(empty.valid());CHECK_EQ(empty.canopyAt(-1,-1),0);
}
VXC_TEST(ecology_fields_validate_and_produce_all_stand_types){
    EcoConfig c;EcoContext context;std::set<int> stands,communities;
    for(int64_t y=-2000000;y<2000000;y+=17000)for(int64_t x=-2000000;x<2000000;x+=19000){
        CHECK(ecoContextAt(42,x,y,c,context));
        stands.insert(int(context.stand));communities.insert(context.community);
        CHECK(context.treeKeepPerMille<=1000);
    }
    CHECK_EQ(stands.size(),5u);CHECK_EQ(communities.size(),4u);
    c.featureRadiusMm=c.featureCellMm;CHECK(!ecoContextAt(42,0,0,c,context));
    c={};CHECK(!ecoContextAt(42,INT64_MAX,0,c,context));
}
VXC_TEST(ecology_thinning_is_order_and_partition_independent){
    std::vector<EcoTree> candidates;uint64_t id=1;
    for(int y=-12;y<=12;++y)for(int x=-12;x<=12;++x)
        candidates.push_back({id++,x*1500,y*1500,1100,4000,700});
    std::set<uint64_t> accepted;
    for(const auto& t:candidates)if(ecoTreeSurvives(42,t,candidates))accepted.insert(t.id);
    CHECK(!accepted.empty());CHECK(accepted.size()<candidates.size()/2);
    std::reverse(candidates.begin(),candidates.end());
    for(const auto& t:candidates){
        CHECK_EQ(ecoTreeSurvives(42,t,candidates),accepted.count(t.id)!=0);
        std::vector<EcoTree> halo;
        for(const auto& n:candidates)if(n.xMm>=t.xMm-2200&&n.xMm<=t.xMm+2200&&
            n.yMm>=t.yMm-2200&&n.yMm<=t.yMm+2200)halo.push_back(n);
        CHECK_EQ(ecoTreeSurvives(42,t,halo),accepted.count(t.id)!=0);
        if(!accepted.count(t.id))continue;
        for(const auto& n:candidates)if(n.id!=t.id&&accepted.count(n.id)){
            const auto dx=t.xMm-n.xMm,dy=t.yMm-n.yMm;CHECK(dx*dx+dy*dy>=2200ll*2200);
        }
    }
}
VXC_TEST(ecology_canopy_drives_cover_and_localized_thickets){
    std::vector<EcoTree> trees={{1,-3000,0,1000,4000,700},{2,0,0,1000,4000,700}};
    const auto center=ecoCanopyAt(0,0,trees);
    CHECK(center>700);CHECK_EQ(ecoCanopyAt(10000,0,trees),0);
    std::reverse(trees.begin(),trees.end());CHECK_EQ(ecoCanopyAt(0,0,trees),center);
    EcoContext c;
    CHECK(ecoCoverWeight(EcoCoverRole::Sun,center,c)<ecoCoverWeight(EcoCoverRole::Sun,0,c));
    CHECK(ecoCoverWeight(EcoCoverRole::Shade,center,c)>ecoCoverWeight(EcoCoverRole::Shade,0,c));
    const auto normal=ecoCoverWeight(EcoCoverRole::Shrub,center,c);
    c.stand=EcoStand::Thicket;c.featureStrengthPerMille=1000;
    CHECK(ecoCoverWeight(EcoCoverRole::Shrub,center,c)>normal);
    CHECK(ecoCoverWeight(EcoCoverRole::Shrub,center,c)<ecoCoverWeight(EcoCoverRole::Shrub,0,c));
    CHECK(ecoCoverWeight(EcoCoverRole::ShadeShrub,center,c)>ecoCoverWeight(EcoCoverRole::ShadeShrub,0,c));
    c.featureStrengthPerMille=0;
    CHECK_EQ(ecoCoverWeight(EcoCoverRole::Shrub,center,c),normal);
    c.stand=EcoStand::Mixed;
    CHECK(ecoCoverWeight(EcoCoverRole::ShadeShrub,center,c)>ecoCoverWeight(EcoCoverRole::ShadeShrub,0,c));
    CHECK_EQ(ecoCoverWeight(EcoCoverRole::Inert,center,c),ecoCoverWeight(EcoCoverRole::Inert,0,c));
}
VXC_TEST(ecology_features_have_irregular_bounded_feathered_footprints){
    EcoConfig c;c.ancientPerMille=1000;c.thicketPerMille=0;
    EcoConfig base=c;base.ancientPerMille=0;
    int feathered=0,asymmetric=0;
    for(int cell=-4;cell<=4;++cell){
        const auto h=hash2(42,cell,0,CH_ECO_FEATURE);
        const int64_t cx=int64_t(cell)*c.featureCellMm+c.featureCellMm/4+int64_t(h%uint64_t(c.featureCellMm/2));
        const int64_t cy=c.featureCellMm/4+int64_t((h>>32)%uint64_t(c.featureCellMm/2));
        for(int offset=2000;offset<c.featureRadiusMm;offset+=2000){
            EcoContext a,b,ordinary;
            CHECK(ecoContextAt(42,cx+offset,cy,c,a));
            CHECK(ecoContextAt(42,cx,cy+offset,c,b));
            if(a.featureStrengthPerMille!=b.featureStrengthPerMille)++asymmetric;
            if(a.featureStrengthPerMille>0&&a.featureStrengthPerMille<1000){
                ++feathered;CHECK(ecoContextAt(42,cx+offset,cy,base,ordinary));
                CHECK(a.targetHeightPerMille>=ordinary.targetHeightPerMille);
                CHECK(a.targetHeightPerMille<950);
            }
        }
        EcoContext outside;
        CHECK(ecoContextAt(42,cx+c.featureRadiusMm,cy,c,outside));
        CHECK_EQ(outside.featureStrengthPerMille,0);
    }
    CHECK(feathered>10);CHECK(asymmetric>30);
}
VXC_TEST(ecology_stands_respond_to_measured_terrain_without_expanding_features){
    EcoConfig c;c.ancientPerMille=1000;c.thicketPerMille=0;
    uint64_t flatDensity=0,steepDensity=0,hollowHeight=0,ridgeHeight=0;
    int flatGroves=0,steepGroves=0;
    for(int y=-160000;y<160000;y+=8000)for(int x=-160000;x<160000;x+=8000){
        EcoContext flat,unknown,steep,hollow,ridge;
        CHECK(ecoContextAt(42,x,y,c,flat));
        CHECK(ecoContextAt(42,x,y,c,unknown,{0,-1,-1,-1}));
        CHECK_EQ(flat.targetHeightPerMille,unknown.targetHeightPerMille);
        CHECK(flat.stand==unknown.stand);
        CHECK(ecoContextAt(42,x,y,c,steep,{1000,80,220,100}));
        CHECK(ecoContextAt(42,x,y,c,hollow,{80,190,100,0}));
        CHECK(ecoContextAt(42,x,y,c,ridge,{80,60,200,0}));
        flatDensity+=flat.treeKeepPerMille;steepDensity+=steep.treeKeepPerMille;
        hollowHeight+=hollow.targetHeightPerMille;ridgeHeight+=ridge.targetHeightPerMille;
        flatGroves+=flat.stand==EcoStand::Ancient;steepGroves+=steep.stand==EcoStand::Ancient;
        CHECK(steep.featureStrengthPerMille<=flat.featureStrengthPerMille);
    }
    CHECK(flatGroves>0);CHECK_EQ(steepGroves,0);
    CHECK(steepDensity<flatDensity);CHECK(hollowHeight>ridgeHeight);
}
VXC_TEST(ecology_variants_respect_publication_size_and_identity){
    std::vector<EcoVariant> variants={{1,7,3000,true},{2,9,12000,true},{3,11,21000,true},{4,12,12000,false}};
    for(uint64_t seed=0;seed<100;++seed){
        const auto* young=ecoChooseVariant(seed,123,3000,variants);
        const auto* old=ecoChooseVariant(seed,123,21000,variants);
        CHECK(young&&old);CHECK_EQ(young->seedIndex,7);CHECK_EQ(old->seedIndex,11);
        const auto before=ecoChooseVariant(seed,123,12000,variants)->stableId;
        std::reverse(variants.begin(),variants.end());
        CHECK_EQ(ecoChooseVariant(seed,123,12000,variants)->stableId,before);
    }
    for(auto& v:variants)v.published=false;
    CHECK(ecoChooseVariant(0,0,12000,variants)==nullptr);
}
VXC_TEST(ecology_growth_forms_follow_stands_without_losing_seed_diversity){
    for(bool woody:{true,false}){
        std::vector<EcoVariant> variants={{1,0,12000,true},{2,1,12000,true},{3,2,12000,true}};
        variants[0].growthForm=woody?EcoGrowthForm::Open:EcoGrowthForm::Spreading;
        variants[1].growthForm=woody?EcoGrowthForm::Woodland:EcoGrowthForm::Compact;
        variants[2].growthForm=woody?EcoGrowthForm::Edge:EcoGrowthForm::Leaning;
        for(auto stand:{EcoStand::Open,EcoStand::Mixed,EcoStand::Thicket,EcoStand::Young}){
            const auto preferred=stand==EcoStand::Open?variants[0].growthForm:
                woody?(stand==EcoStand::Thicket?EcoGrowthForm::Edge:EcoGrowthForm::Woodland):
                (stand==EcoStand::Thicket?EcoGrowthForm::Spreading:stand==EcoStand::Young?EcoGrowthForm::Leaning:EcoGrowthForm::Compact);
            int matches=0;
            for(uint64_t seed=0;seed<1000;++seed){
                const auto* selected=ecoChooseVariant(seed,123,12000,variants,stand);
                if(selected->growthForm==preferred)++matches;
                const auto identity=selected->stableId;
                std::reverse(variants.begin(),variants.end());
                CHECK_EQ(ecoChooseVariant(seed,123,12000,variants,stand)->stableId,identity);
            }
            CHECK(matches>800);CHECK(matches<1000);
        }
        for(auto& v:variants)v.published=false;
        variants[0].published=true;
        for(uint64_t seed=0;seed<100;++seed)
            CHECK_EQ(ecoChooseVariant(seed,123,12000,variants,EcoStand::Open)->stableId,variants[0].stableId);
    }
}
