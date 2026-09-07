#include "voxelcore/assetauthority.h"
#include "voxelcore/assetauthoritycoordinator.h"
#include <thread>
#include "vxctest.h"
using namespace vxc;
namespace {

// A minimal VXA v3 blob, so the composition is tested against a grid with
// KNOWN voxels rather than against whatever a fixture happens to contain.
// Deliberately the same encoder shape test_assetgrid.cpp uses for its synthetic
// cases; the fixture tests there are what validate the reader itself.
void put32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 24));
}

// A solid nx*ny*nz block of `mat`, anchored so its base sits on the anchor
// voxel: origin (0,0,0) means local (0,0,0) is the anchor itself.
std::vector<uint8_t> solidBlock(int32_t nx, int32_t ny, int32_t nz, MaterialId mat,
                                uint32_t voxelSizeMm, int32_t ox = 0, int32_t oy = 0,
                                int32_t oz = 0) {
    std::vector<uint8_t> b;
    put32(b, kVxaMagic);
    put32(b, kVxaVersion);
    put32(b, uint32_t(ox));
    put32(b, uint32_t(oy));
    put32(b, uint32_t(oz));
    put32(b, uint32_t(nx));
    put32(b, uint32_t(ny));
    put32(b, uint32_t(nz));
    put32(b, voxelSizeMm);  // v2 -- BEFORE the run count, per forge/vxa.py
    put32(b, 1u);           // runCount
    put32(b, 0u);           // partRunCount (v3)
    put32(b, 0u);           // jointCount (v3)
    // one run covering the whole box
    b.push_back(uint8_t(mat));
    put32(b, uint32_t(nx * ny * nz));
    return b;
}

// A bank source over one grid, for every (bankId, seedIndex).
class OneGridBank : public IAssetBankSource {
public:
    explicit OneGridBank(const AssetGrid* g) : g_(g) {}
    const AssetGrid* bankGrid(uint16_t, uint16_t) const override { return g_; }

private:
    const AssetGrid* g_;
};

// A bank source that has nothing, to check the "failed to load" path leaves the
// world it was going to stand in intact.
class EmptyBank : public IAssetBankSource {
public:
    const AssetGrid* bankGrid(uint16_t, uint16_t) const override { return nullptr; }
};

AssetSpecies pillarSpecies(uint8_t layer, int32_t heightMm, uint32_t voxelSizeMm) {
    AssetSpecies s;
    s.bankId = 1;
    s.layer = layer;
    for (int b = 0; b < kBiomeCount; ++b) s.weightPerMille[b] = 1000;
    s.elevMinMm = -1'000'000;
    s.elevMaxMm = 9'000'000;
    s.slopeMaxMmPerM = 100000;
    s.heightMm = heightMm;
    s.voxelSizeMm = voxelSizeMm;
    return s;
}

AssetColumnFacts flatGround(int32_t surfaceMm) {
    AssetColumnFacts c;
    c.known = true;
    c.anchorSolid = true;
    c.biome = TEMPERATE_FOREST;
    c.surfaceMm = surfaceMm;
    c.slopeMmPerM = 0;
    return c;
}

// A layer with a site in every cell, so nothing in these tests turns on a hash
// draw. test_assetplacement.cpp records what a hash-dependent test costs.
AssetLayer everyCell(int32_t cellMm, int32_t heightMm, int32_t radiusMm, bool terrain = true) {
    AssetLayer L{};
    L.cellMm = cellMm;
    L.maxHeightMm = heightMm;
    L.maxDepthMm = 0;
    L.maxRadiusMm = radiusMm;
    L.densityPerMille = 1000;
    L.seedCount = 1;
    L.terrainLattice = terrain;
    return L;
}

constexpr uint64_t kSeed = 1234ull;

} // namespace
namespace {
struct FixtureIdentity : IAssetAuthorityIdentity {
    const AssetField* field=nullptr;
    std::string catalogIdentity(const AssetField& f) const override{return &f==field?"fixture-catalog":"";}
    // Test seam only: production adapter must call canonical MD5 GridContentHash.
    std::string contentHash(const AssetGrid& g) const override {
        return g.at(0,0,0)==MAT_BARK?"11111111111111111111111111111111":"22222222222222222222222222222222";
    }
};
}
VXC_TEST(authority_stationary_carve_adjacent_edit_and_binding) {
    SyntheticTileSampler tiles(kSeed);World<16> world(kSeed,tiles,"fixture-provider");
    AssetGrid source;CHECK(source.parse(solidBlock(1,1,30,MAT_BARK,100))==AssetParseError::kOk);
    OneGridBank bank(&source);AssetField field;field.setSeed(kSeed);
    const AssetLayer layers[]{everyCell(3200,3000,0)};const AssetSpecies species[]{pillarSpecies(0,3000,100)};
    field.setLayers(layers,1);field.setSpecies(species,1);field.setBankSource(&bank);world.setAssetField(&field);
    FixtureIdentity identity;identity.field=&field;
    auto facts=[&](int64_t x,int64_t y){return assetColumnFactsFromSample(world.amplifier().columnCached(x,y));};
    std::vector<AssetInstance> instances;CHECK(assetAuthorityInstances(field,{-32,-32,31,31},facts,instances));
    auto ordered=field.resolveForCompose(instances);CHECK(!ordered.empty());if(ordered.empty())return;
    const auto r=ordered.front();AssetCandidateBounds b;CHECK(assetCandidateBounds(r,b));
    // Re-resolve full selected footprint so clipping and capture share winners.
    CHECK(assetAuthorityInstances(field,{b.x0,b.y0,b.x1,b.y1},facts,instances));ordered=field.resolveForCompose(instances);
    size_t selected=ordered.size();for(size_t i=0;i<ordered.size();++i)if(ordered[i].anchorVx==r.anchorVx&&ordered[i].anchorVy==r.anchorVy)selected=i;
    CHECK(selected<ordered.size());if(selected==ordered.size())return;
    std::vector<uint8_t> clipped;CHECK(assetBuildCandidateVxa(ordered,selected,[&](int64_t x,int64_t y){return world.amplifier().columnCached(x,y);},clipped));
    AssetGrid projection;CHECK(projection.parse(clipped)==AssetParseError::kOk);
    AssetProvenance p;p.worldSeed=kSeed;p.providerFingerprint=assetAuthorityFingerprint("fixture-provider:worldgen:"+std::to_string(kWorldGenVersion));
    p.catalogFingerprint=assetAuthorityFingerprint("fixture-catalog:"+identity.contentHash(source));
    p.anchorVx=r.anchorVx;p.anchorVy=r.anchorVy;p.anchorVz=r.anchorVz;p.layer=r.layer;p.yawQuarter=r.yawQuarter;p.bankId=r.bankId;p.seedIndex=r.seedIndex;
    std::string error;auto capture=[&](const auto& w,const auto& prov,uint64_t rev){return StationaryAssetAuthority<16>::capture(w,prov,source,projection,7,1,rev,identity,error);};
    auto view=capture(world,p,1);CHECK(bool(view));if(!view){std::fprintf(stderr,"capture: %s\n",error.c_str());return;}
    const int64_t x=b.x0,y=b.y0,z=b.z1;AssetAuthoritySample hit;CHECK(view->sampleAt(x,y,z,hit));CHECK_EQ(hit.material,MAT_BARK);CHECK(hit.owner==assetObjectId(p));
    CHECK_EQ(world.materialAt(x,y,z),MAT_BARK); // World default is unchanged.
    auto carved=view->carve(7,x,y,z);CHECK(bool(carved));if(!carved)return;
    CHECK(carved->sampleAt(x,y,z,hit));CHECK_EQ(hit.material,MAT_AIR);
    // Adjacent terrain edit materializes solely from captured terrain authority.
    auto edited=carved->editTerrain(carved->generation(),x+1,y,z,MAT_ROCK);CHECK(bool(edited));if(!edited)return;
    CHECK(edited->sampleAt(x,y,z,hit));CHECK_EQ(hit.material,MAT_AIR);
    CHECK(edited->sampleAt(x,y,z-1,hit));CHECK_EQ(hit.material,MAT_BARK);CHECK(hit.owner==assetObjectId(p));
    CHECK(edited->sampleAt(x+1,y,z,hit));CHECK_EQ(hit.material,MAT_ROCK);CHECK(hit.owner==AssetObjectId{});
    const auto key=ChunkMap<16>::keyForVoxel(x,y,z);Brick<16> brick;CHECK(edited->makeBrick(key,brick));
    for(int dz=0;dz<16;++dz)for(int dy=0;dy<16;++dy)for(int dx=0;dx<16;++dx){MaterialId mat;CHECK(edited->terrainAt(int64_t(key.x)*16+dx,int64_t(key.y)*16+dy,int64_t(key.z)*16+dz,mat));CHECK_EQ(mat,brick.get(dx,dy,dz));}
    auto air=edited->editTerrain(edited->generation(),x,y,z-1,MAT_AIR);CHECK(bool(air));CHECK(air->sampleAt(x,y,z-1,hit));CHECK_EQ(hit.material,MAT_AIR);
    CHECK(view->sampleAt(x,y,z,hit));CHECK_EQ(hit.material,MAT_BARK); // Old generation remains coherent.
    CHECK(!edited->carve(7,x,y,z-1));CHECK(!view->sampleAt(view->bounds().x1+1,y,z,hit));
    World<16> other(kSeed,tiles,"other-provider");other.setAssetField(&field);CHECK(!capture(other,p,1));
    auto bad=p;bad.catalogFingerprint^=1;CHECK(!capture(world,bad,1));bad=p;bad.yawQuarter=4;CHECK(!capture(world,bad,1));
    bad=p;bad.anchorVx=INT64_MAX;CHECK(!capture(world,bad,1));bad=p;bad.anchorVy=INT64_MIN;CHECK(!capture(world,bad,1));CHECK(!capture(world,p,0));
    identity.field=nullptr;CHECK(!capture(world,p,1));identity.field=&field;
    world.setVoxel(x+1,y,z,MAT_ROCK);CHECK(!capture(world,p,1));
    source=AssetGrid{};projection=AssetGrid{};CHECK(view->sampleAt(x,y,z,hit));CHECK_EQ(hit.material,MAT_BARK);
}
VXC_TEST(authority_site_preflight_refuses_before_columns_and_ignores_detail) {
    AssetGrid grid;CHECK(grid.parse(solidBlock(1,1,1,MAT_BARK,100))==AssetParseError::kOk);OneGridBank bank(&grid);
    AssetField field;field.setSeed(kSeed);field.setBankSource(&bank);
    AssetLayer layers[]{everyCell(1,100,100000),everyCell(1,100,100000,false)};
    const AssetSpecies species[]{pillarSpecies(0,100,100)};field.setSpecies(species,1);field.setLayers(layers,2);
    int calls=0;std::vector<AssetInstance> out;
    auto facts=[&](int64_t,int64_t){++calls;return flatGround(100000);};
    CHECK(!assetAuthorityInstances(field,{0,0,31,31},facts,out));CHECK_EQ(calls,0);CHECK(out.empty());
    layers[0]=everyCell(3200,100,0);field.setLayers(layers,2);
    CHECK(assetAuthorityInstances(field,{0,0,31,31},facts,out));CHECK(calls>0);
    field.setLayers(layers,1);
    const auto ordinary=field.instancesForRect({0,0,31,31},facts,true);CHECK_EQ(out.size(),ordinary.size());
}

VXC_TEST(authority_original_winner_preserves_overlap_order) {
    AssetGrid rock,wood;CHECK(rock.parse(solidBlock(2,1,1,MAT_ROCK,100))==AssetParseError::kOk);
    CHECK(wood.parse(solidBlock(2,1,1,MAT_BARK,100))==AssetParseError::kOk);
    for(uint8_t yaw=0;yaw<4;++yaw) {
        AssetField::ResolvedAssetInstance a;a.grid=&rock;a.anchorVx=-2;a.anchorVy=-3;a.yawQuarter=yaw;
        auto b=a;b.grid=&wood;std::vector<AssetField::ResolvedAssetInstance> ordered{a,b};
        std::vector<AssetCandidateBounds> boxes(2);CHECK(assetCandidateBounds(a,boxes[0]));CHECK(assetCandidateBounds(b,boxes[1]));
        CHECK_EQ(assetAuthorityWinner(ordered,boxes,boxes[0].x0,boxes[0].y0,boxes[0].z0),size_t(0));
        // Ownership of index0 must return air, never re-search index1; ownership
        // of index1 cannot erase index0. Same winner selector used by capture.
        ordered[0].suppressTerrainRender=true;
        CHECK_EQ(assetAuthorityWinner(ordered,boxes,boxes[0].x0,boxes[0].y0,boxes[0].z0),size_t(0));
    }
}

VXC_TEST(authority_capture_overlap_and_base_terrain) {
    SyntheticTileSampler tiles(kSeed);World<16> world(kSeed,tiles,"fixture-provider");
    AssetGrid first,last;CHECK(first.parse(solidBlock(1,1,30,MAT_BARK,100))==AssetParseError::kOk);
    CHECK(last.parse(solidBlock(1,1,30,MAT_ROCK,100))==AssetParseError::kOk);
    struct Banks : IAssetBankSource {const AssetGrid* a;const AssetGrid* b;
        const AssetGrid* bankGrid(uint16_t bank,uint16_t) const override{return bank==1?a:b;}} bank;
    bank.a=&first;bank.b=&last;AssetField field;field.setSeed(kSeed);field.setBankSource(&bank);
    AssetLayer layers[]{everyCell(3200,6000,3200),everyCell(3200,6000,3200)};
    AssetSpecies species[]{pillarSpecies(0,6000,100),pillarSpecies(1,6000,100)};species[1].bankId=2;
    field.setLayers(layers,2);field.setSpecies(species,2);world.setAssetField(&field);
    AssetInstance anchors[2];
    for(int i=0;i<2;++i){AssetSite site;CHECK(assetSiteInCell(kSeed,layers[i],i,0,0,site));
        const auto facts=assetColumnFactsFromSample(world.amplifier().columnCached(floorDiv(site.anchorXMm,int64_t(100)),floorDiv(site.anchorYMm,int64_t(100))));
        CHECK(assetResolveSite(kSeed,layers,2,species,2,site,facts,anchors[i]));}
    const int64_t ax=floorDiv(anchors[0].anchorXMm,int64_t(100)),ay=floorDiv(anchors[0].anchorYMm,int64_t(100));
    const int32_t dx=int32_t(ax-floorDiv(anchors[1].anchorXMm,int64_t(100))),dy=int32_t(ay-floorDiv(anchors[1].anchorYMm,int64_t(100)));
    int32_t ox=dx,oy=dy;
    switch(anchors[1].yawQuarter){case 1:ox=dy;oy=-dx;break;case 2:ox=-dx;oy=-dy;break;case 3:ox=-dy;oy=dx;break;default:break;}
    const int32_t oz=int32_t(anchors[0].anchorVz-anchors[1].anchorVz);
    CHECK(last.parse(solidBlock(1,1,30,MAT_ROCK,100,ox,oy,oz))==AssetParseError::kOk);
    FixtureIdentity identity;identity.field=&field;std::string error;
    for(int selectedLayer=0;selectedLayer<2;++selectedLayer) {
        const auto& inst=anchors[selectedLayer];const auto& source=selectedLayer==0?first:last;
        AssetProvenance p;p.worldSeed=kSeed;p.providerFingerprint=assetAuthorityFingerprint("fixture-provider:worldgen:"+std::to_string(kWorldGenVersion));
        p.catalogFingerprint=assetAuthorityFingerprint("fixture-catalog:"+identity.contentHash(source));
        p.anchorVx=floorDiv(inst.anchorXMm,int64_t(100));p.anchorVy=floorDiv(inst.anchorYMm,int64_t(100));p.anchorVz=inst.anchorVz;
        p.bankId=inst.bankId;p.seedIndex=inst.seedIndex;p.layer=inst.layer;p.yawQuarter=inst.yawQuarter;
        AssetGrid projection;
        if(selectedLayer==1) { // Later source owns zero voxels under the first.
            CHECK(projection.parse(solidBlock(1,1,30,MAT_AIR,100,source.rotatedOriginX(p.yawQuarter),source.rotatedOriginY(p.yawQuarter),source.originZ()))==AssetParseError::kOk);
        } else {
            std::vector<AssetInstance> instances;CHECK(assetAuthorityInstances(field,{ax,ay,ax,ay},[&](int64_t x,int64_t y){return assetColumnFactsFromSample(world.amplifier().columnCached(x,y));},instances));
            const auto ordered=field.resolveForCompose(instances);size_t index=ordered.size();
            for(size_t i=0;i<ordered.size();++i)if(ordered[i].layer==0&&ordered[i].anchorVx==ax&&ordered[i].anchorVy==ay)index=i;
            std::vector<uint8_t> clipped;CHECK(assetBuildCandidateVxa(ordered,index,[&](int64_t x,int64_t y){return world.amplifier().columnCached(x,y);},clipped));CHECK(projection.parse(clipped)==AssetParseError::kOk);
        }
        auto view=StationaryAssetAuthority<16>::capture(world,p,source,projection,1,1,1,identity,error);
        CHECK(bool(view));if(!view){std::fprintf(stderr,"overlap capture: %s\n",error.c_str());continue;}
        AssetAuthoritySample hit;const int64_t z=anchors[0].anchorVz+29;
        CHECK(view->sampleAt(ax,ay,z,hit));CHECK_EQ(hit.material,MAT_BARK);
        if(selectedLayer==0) {
            CHECK(hit.owner==assetObjectId(p));auto carved=view->carve(1,ax,ay,z);CHECK(bool(carved));
            CHECK(carved->sampleAt(ax,ay,z,hit));CHECK_EQ(hit.material,MAT_AIR); // rock never revealed
        }else {CHECK(hit.owner==AssetObjectId{});CHECK(!view->carve(1,ax,ay,z));}
        const int64_t ground=topSolidVoxelZ(world.amplifier().columnCached(ax,ay).surfaceMm);
        CHECK(view->sampleAt(ax,ay,ground,hit));CHECK_EQ(hit.material,world.amplifier().materialAt(ax,ay,ground));CHECK(hit.owner==AssetObjectId{});
    }
}

VXC_TEST(authority_coordinator_bounds_leases_and_rejects_stale_publication) {
    SyntheticTileSampler tiles(kSeed);World<16> world(kSeed,tiles,"fixture-provider");
    AssetGrid source;CHECK(source.parse(solidBlock(1,1,30,MAT_BARK,100))==AssetParseError::kOk);
    OneGridBank bank(&source);AssetField field;field.setSeed(kSeed);
    const AssetLayer layers[]{everyCell(3200,3000,0)};const AssetSpecies species[]{pillarSpecies(0,3000,100)};
    field.setLayers(layers,1);field.setSpecies(species,1);field.setBankSource(&bank);world.setAssetField(&field);
    FixtureIdentity identity;identity.field=&field;
    auto facts=[&](int64_t x,int64_t y){return assetColumnFactsFromSample(world.amplifier().columnCached(x,y));};
    std::vector<AssetInstance> instances;CHECK(assetAuthorityInstances(field,{-32,-32,31,31},facts,instances));
    auto ordered=field.resolveForCompose(instances);CHECK(!ordered.empty());if(ordered.empty())return;
    const auto r=ordered.front();AssetCandidateBounds b;CHECK(assetCandidateBounds(r,b));
    // Re-resolve full selected footprint so clipping and capture share winners.
    CHECK(assetAuthorityInstances(field,{b.x0,b.y0,b.x1,b.y1},facts,instances));ordered=field.resolveForCompose(instances);
    size_t selected=ordered.size();for(size_t i=0;i<ordered.size();++i)if(ordered[i].anchorVx==r.anchorVx&&ordered[i].anchorVy==r.anchorVy)selected=i;
    CHECK(selected<ordered.size());if(selected==ordered.size())return;
    std::vector<uint8_t> clipped;CHECK(assetBuildCandidateVxa(ordered,selected,[&](int64_t x,int64_t y){return world.amplifier().columnCached(x,y);},clipped));
    AssetGrid projection;CHECK(projection.parse(clipped)==AssetParseError::kOk);
    AssetProvenance p;p.worldSeed=kSeed;p.providerFingerprint=assetAuthorityFingerprint("fixture-provider:worldgen:"+std::to_string(kWorldGenVersion));
    p.catalogFingerprint=assetAuthorityFingerprint("fixture-catalog:"+identity.contentHash(source));
    p.anchorVx=r.anchorVx;p.anchorVy=r.anchorVy;p.anchorVz=r.anchorVz;p.layer=r.layer;p.yawQuarter=r.yawQuarter;p.bankId=r.bankId;p.seedIndex=r.seedIndex;
    std::string error;auto capture=[&](const auto& w,const auto& prov,uint64_t rev){return StationaryAssetAuthority<16>::capture(w,prov,source,projection,7,1,rev,identity,error);};

    using Coordinator=StationaryAssetAuthorityCoordinator<16>;
    const auto charge=Coordinator::creditBytes();
    Coordinator coordinator(charge*3,3);
    auto initial=coordinator.prepareInitial(world,p,source,projection,7,1,1,identity,error);
    CHECK(bool(initial));CHECK_EQ(coordinator.usage().generations,size_t(1));
    CHECK(coordinator.publish(initial));CHECK(!coordinator.publish(initial));
    auto old=coordinator.visible();CHECK(bool(old));if(!old)return;
    const int64_t x=b.x0,y=b.y0,z=b.z1;
    auto first=coordinator.prepareCarve(old,x,y,z);
    auto stale=coordinator.prepareTerrainEdit(old,x+1,y,z,MAT_ROCK);
    CHECK(bool(first));CHECK(bool(stale));CHECK_EQ(coordinator.usage().generations,size_t(3));
    CHECK_EQ(coordinator.usage().bytes,charge*3);
    CHECK(!coordinator.prepareTerrainEdit(old,x+2,y,z,MAT_ROCK));
    CHECK(!old->editTerrain(old->generation(),x+2,y,z,MAT_ROCK)); // direct edit cannot bypass credits
    CHECK(coordinator.publish(first));CHECK(!coordinator.valid(stale));CHECK(!coordinator.publish(stale));
    CHECK(coordinator.cancel(stale));CHECK_EQ(coordinator.usage().generations,size_t(2));
    CHECK(!coordinator.prepareCarve(old,x,y,z-1)); // exact visible identity required
    old.reset();CHECK_EQ(coordinator.usage().generations,size_t(1));
    auto next=coordinator.prepareTerrainEdit(coordinator.visible(),x+1,y,z,MAT_ROCK);
    CHECK(bool(next));CHECK(coordinator.cancel(next));CHECK(!coordinator.cancel(next));CHECK_EQ(coordinator.usage().generations,size_t(1));
    auto workerLease=coordinator.visible();
    auto workerNext=coordinator.prepareTerrainEdit(workerLease,x+1,y,z,MAT_ROCK);
    CHECK(coordinator.publish(workerNext));CHECK_EQ(coordinator.usage().generations,size_t(2));
    std::thread releaseWorker([held=std::move(workerLease)]() mutable {held.reset();});releaseWorker.join();
    CHECK_EQ(coordinator.usage().generations,size_t(1));
    // Repeatedly retaining consumed/cancelled ticket handles retains no candidate arrays.
    std::vector<Coordinator::Ticket> retained;
    for(int i=0;i<20;++i){auto t=coordinator.prepareTerrainEdit(coordinator.visible(),x+1,y,z,MAT_ROCK);CHECK(bool(t));CHECK(coordinator.cancel(t));retained.push_back(t);}
    CHECK_EQ(coordinator.usage().generations,size_t(1));
    Coordinator other(charge*3,3);auto wrong=other.prepareInitial(world,p,source,projection,7,1,1,identity,error);
    CHECK(bool(wrong));CHECK(!coordinator.publish(wrong));CHECK(other.cancel(wrong));
    Coordinator tiny(charge-1,3);CHECK(!tiny.prepareInitial(world,p,source,projection,7,1,1,identity,error));CHECK_EQ(tiny.usage().bytes,uint64_t(0));
    auto bad=p;bad.providerFingerprint^=1;
    CHECK(!other.prepareInitial(world,bad,source,projection,7,1,1,identity,error));CHECK_EQ(other.usage().generations,size_t(0));
    // A raw independently captured view is not publishable/admissible as expected base.
    auto uncoordinated=capture(world,p,1);CHECK(bool(uncoordinated));CHECK(!coordinator.prepareCarve(uncoordinated,x,y,z));
    typename Coordinator::Ref orphan;
    {
        Coordinator temporary(charge*2,2);auto t=temporary.prepareInitial(world,p,source,projection,7,1,1,identity,error);
        CHECK(temporary.publish(t));orphan=temporary.visible();
    }
    AssetAuthoritySample hit;CHECK(orphan->sampleAt(x,y,z,hit));CHECK_EQ(hit.material,MAT_BARK);
    CHECK(!orphan->editTerrain(orphan->generation(),x+1,y,z,MAT_ROCK)); // closed ledger survives owner
    orphan.reset();
}
