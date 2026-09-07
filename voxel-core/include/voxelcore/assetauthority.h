#pragma once
// Bounded stationary authority foundation. Not installed into World or UE.
#include "voxelcore/world.h"
#include "voxelcore/assetcandidate.h"
#include <memory>
#include <string>
namespace vxc {
// Trusted host binding service: resolve the actual loaded field's catalog
// identity and use the existing canonical GridContentHash MD5. Unknown fields
// must return empty. This is a digest/catalog adapter, never an owns callback.
struct IAssetAuthorityIdentity {
    virtual ~IAssetAuthorityIdentity()=default;
    virtual std::string catalogIdentity(const AssetField&) const=0;
    virtual std::string contentHash(const AssetGrid&) const=0;
};
inline uint64_t assetAuthorityFingerprint(const std::string& s) {
    uint64_t h=14695981039346656037ull;
    for(unsigned char c:s)h=(h^c)*1099511628211ull;
    return h;
}
// Terrain-only enumeration with checked preallocation and bounded site visits.
// Ordering is exactly the terrain subsequence of AssetField::instancesForRect.
template<class Facts>
bool assetAuthorityInstances(const AssetField& field,const AssetVoxelRect& rect,
    Facts&& facts,std::vector<AssetInstance>& out,size_t maxSites=8192) {
    out.clear();constexpr int64_t Limit=int64_t(1)<<31;
    if(!rect.valid()||rect.vx0 < -Limit||rect.vy0 < -Limit||rect.vx1>Limit||rect.vy1>Limit||maxSites>8192)return false;
    const int64_t x0=rect.vx0*kVoxelSizeMm,x1=rect.vx1*kVoxelSizeMm+kVoxelSizeMm-1;
    const int64_t y0=rect.vy0*kVoxelSizeMm,y1=rect.vy1*kVoxelSizeMm+kVoxelSizeMm-1;
    struct Range {int64_t x0,x1,y0,y1;};Range ranges[kAssetLayerCount]{};size_t visits=0;
    const auto& layers=field.layers();
    for(size_t i=0;i<layers.size();++i) {
        const auto& l=layers[i];if(!l.terrainLattice||l.cellMm<=0)continue;
        if(l.maxRadiusMm<0)return false;
        const int64_t r=l.maxRadiusMm;
        auto& a=ranges[i];a={floorDiv(x0-r,int64_t(l.cellMm)),floorDiv(x1+r,int64_t(l.cellMm)),floorDiv(y0-r,int64_t(l.cellMm)),floorDiv(y1+r,int64_t(l.cellMm))};
        const uint64_t nx=uint64_t(a.x1-a.x0)+1,ny=uint64_t(a.y1-a.y0)+1;
        if(nx>maxSites||ny>maxSites||nx*ny>maxSites-visits)return false;
        visits+=size_t(nx*ny);
    }
    out.reserve(visits);
    for(size_t i=0;i<layers.size();++i) {
        const auto& l=layers[i];if(!l.terrainLattice||l.cellMm<=0)continue;const auto& a=ranges[i];
        for(int64_t y=a.y0;y<=a.y1;++y)for(int64_t x=a.x0;x<=a.x1;++x) {
            AssetSite site;if(!assetSiteInCell(field.seed(),l,int(i),x,y,site))continue;
            const int64_t r=l.maxRadiusMm;
            if(site.anchorXMm+r<x0||site.anchorXMm-r>x1||site.anchorYMm+r<y0||site.anchorYMm-r>y1)continue;
            AssetInstance inst;
            if(assetResolveSite(field.seed(),layers.data(),int(layers.size()),field.species().data(),int(field.species().size()),site,
                facts(floorDiv(site.anchorXMm,int64_t(kVoxelSizeMm)),floorDiv(site.anchorYMm,int64_t(kVoxelSizeMm))),inst))out.push_back(inst);
        }
    }
    return true;
}
inline size_t assetAuthorityWinner(const std::vector<AssetField::ResolvedAssetInstance>& ordered,
    const std::vector<AssetCandidateBounds>& boxes,int64_t x,int64_t y,int64_t z) {
    if(boxes.size()!=ordered.size())return ordered.size();
    for(size_t i=0;i<ordered.size();++i)if(assetCandidateMaterial(ordered[i],boxes[i],x,y,z)!=MAT_AIR)return i;
    return ordered.size();
}
struct AssetAuthoritySample {MaterialId material=MAT_AIR;AssetObjectId owner{};uint64_t generation=0,objectRevision=0;};
template<int B> class StationaryAssetAuthorityCoordinator;
// A coordinated view must reserve a sibling credit before any edit array copy.
// Last-view destruction releases arrays before this lease.
struct IAssetAuthorityRetention {
    virtual ~IAssetAuthorityRetention()=default;
    virtual std::shared_ptr<const IAssetAuthorityRetention> reserveSibling() const=0;
};
template<int B> class StationaryAssetAuthority {
public:
    using Ref=std::shared_ptr<const StationaryAssetAuthority>;
    StationaryAssetAuthority(const StationaryAssetAuthority&)=delete;
    StationaryAssetAuthority& operator=(const StationaryAssetAuthority&)=delete;
    uint64_t generation() const{return generation_;}
    uint64_t objectRevision() const{return revision_;}
    const AssetProvenance& provenance() const{return provenance_;}
    const AssetCandidateBounds& bounds() const{return domain_;}
    size_t retainedMaterialBytes() const{return (terrain_->size()+source_->size()+projection_->size())*sizeof(MaterialId)+edits_->size()*sizeof(Override);}
    bool terrainAt(int64_t x,int64_t y,int64_t z,MaterialId& out) const {
        size_t i;if(!index(x,y,z,i))return false;out=(*edits_)[i].present?(*edits_)[i].material:(*terrain_)[i];return true;
    }
    bool sampleAt(int64_t x,int64_t y,int64_t z,AssetAuthoritySample& out) const {
        size_t i;if(!index(x,y,z,i))return false;
        out={MAT_AIR,{},generation_,revision_};
        if((*edits_)[i].present){out.material=(*edits_)[i].material;return true;}
        out.material=(*terrain_)[i];
        if(out.material==MAT_AIR&&(*projection_)[i]!=MAT_AIR){out.material=(*projection_)[i];out.owner=assetObjectId(provenance_);}
        return true;
    }
    bool makeBrick(const BrickKey& key,Brick<B>& out) const {
        size_t ignored;
        if(!index(int64_t(key.x)*B,int64_t(key.y)*B,int64_t(key.z)*B,ignored)||
           !index(int64_t(key.x)*B+B-1,int64_t(key.y)*B+B-1,int64_t(key.z)*B+B-1,ignored))return false;
        Brick<B> b;for(int z=0;z<B;++z)for(int y=0;y<B;++y)for(int x=0;x<B;++x){MaterialId m;terrainAt(int64_t(key.x)*B+x,int64_t(key.y)*B+y,int64_t(key.z)*B+z,m);b.set(x,y,z,m);}
        b.tryCollapse();out=std::move(b);return true;
    }
    // Functional edits: old readers retain a consistent old generation.
    Ref carve(uint64_t expectedGeneration,int64_t x,int64_t y,int64_t z) const {
        size_t i;if(expectedGeneration!=generation_||generation_==UINT64_MAX||revision_==UINT64_MAX||!index(x,y,z,i)||(*projection_)[i]==MAT_AIR||(*edits_)[i].present)return {};
        auto out=clone();if(!out)return {};auto p=std::make_shared<std::vector<MaterialId>>(*projection_);(*p)[i]=MAT_AIR;out->projection_=p;++out->revision_;return out;
    }
    Ref editTerrain(uint64_t expectedGeneration,int64_t x,int64_t y,int64_t z,MaterialId m) const {
        size_t i;if(expectedGeneration!=generation_||generation_==UINT64_MAX||!index(x,y,z,i))return {};
        auto out=clone();if(!out)return {};auto e=std::make_shared<std::vector<Override>>(*edits_);(*e)[i]={true,m};out->edits_=e;return out;
    }
    // Construction synchronously borrows a frozen World only here. No provider,
    // bank, World, grid or UObject pointer survives the capture.
    static Ref capture(const World<B>& world,const AssetProvenance& p,const AssetGrid& canonical,
        const AssetGrid& current,uint64_t generation,uint64_t objectRevision,uint64_t projectionRevision,
        const IAssetAuthorityIdentity& identity,std::string& error) {
        error.clear();auto fail=[&](const char* why)->Ref{error=why;return {};};
        const auto* field=world.assetField();const auto& provider=world.log().providerId();
        if(!field||field->empty()||world.amplifier().seed()!=p.worldSeed||field->seed()!=p.worldSeed||provider.empty()||
           !generation||!objectRevision||objectRevision!=projectionRevision||p.yawQuarter>3)return fail("world/identity/revision");
        const auto catalog=identity.catalogIdentity(*field);
        if(catalog.empty()||p.providerFingerprint!=assetAuthorityFingerprint(provider+":worldgen:"+std::to_string(kWorldGenVersion)))return fail("provider/catalog binding");
        AssetField::ResolvedAssetInstance wanted;wanted.grid=&canonical;wanted.anchorVx=p.anchorVx;wanted.anchorVy=p.anchorVy;wanted.anchorVz=p.anchorVz;wanted.yawQuarter=p.yawQuarter;
        AssetCandidateBounds b;if(!assetCandidateBounds(wanted,b)||!current.valid()||!current.onTerrainLattice()||canonical.hasParts()||current.hasParts())return fail("source format");
        if(canonical.sizeX()>64||canonical.sizeY()>64||canonical.sizeZ()>128)return fail("source extent budget");
        if(current.sizeX()!=canonical.rotatedSizeX(p.yawQuarter)||current.sizeY()!=canonical.rotatedSizeY(p.yawQuarter)||current.sizeZ()!=canonical.sizeZ()||
           current.originX()!=canonical.rotatedOriginX(p.yawQuarter)||current.originY()!=canonical.rotatedOriginY(p.yawQuarter)||current.originZ()!=canonical.originZ())return fail("projection transform");
        const auto hash=identity.contentHash(canonical);
        if(hash.size()!=32)return fail("source digest format");
        for(char c:hash)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return fail("source digest format");
        if(p.catalogFingerprint!=assetAuthorityFingerprint(catalog+":"+hash))return fail("catalog/source mismatch");
        // Brick-align and include one neighboring brick on every side for edits.
        AssetCandidateBounds domain{(floorDiv(b.x0,B)-1)*B,(floorDiv(b.y0,B)-1)*B,(floorDiv(b.z0,B)-1)*B,
            (floorDiv(b.x1,B)+2)*B-1,(floorDiv(b.y1,B)+2)*B-1,(floorDiv(b.z1,B)+2)*B-1};
        const uint64_t nx=uint64_t(domain.x1-domain.x0+1),ny=uint64_t(domain.y1-domain.y0+1),nz=uint64_t(domain.z1-domain.z0+1);
        constexpr uint64_t MaxCells=1024*1024;
        if(nx>MaxCells||ny>MaxCells||nz>MaxCells||nx*ny>MaxCells||nx*ny*nz>MaxCells)return fail("capture cell budget");
        for(int64_t z=floorDiv(domain.z0,B);z<=floorDiv(domain.z1,B);++z)
        for(int64_t y=floorDiv(domain.y0,B);y<=floorDiv(domain.y1,B);++y)
        for(int64_t x=floorDiv(domain.x0,B);x<=floorDiv(domain.x1,B);++x) {
            if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX||z<INT32_MIN||z>INT32_MAX)return fail("brick coordinate bounds");
            const BrickKey key{int32_t(x),int32_t(y),int32_t(z)};
            if(world.editedBricks().find(key))return fail("preexisting terrain overlay");
            if constexpr(World<B>::kCraftSupported)if(world.isPromoted(key))return fail("preexisting craft promotion");
        }
        std::vector<AssetInstance> instances;
        if(!assetAuthorityInstances(*field,{domain.x0,domain.y0,domain.x1,domain.y1},[&](int64_t x,int64_t y){
            return assetColumnFactsFromSample(world.amplifier().columnCached(x,y),world.assetChannelsAt(x,y));},instances))return fail("site preflight budget");
        const auto ordered=field->resolveForCompose(instances);std::vector<AssetCandidateBounds> boxes;boxes.reserve(ordered.size());
        size_t selected=ordered.size();
        // Bound worst-case composition BEFORE voxel iteration and dense allocation.
        if(ordered.size()>64||nx*ny*nz>64ull*1024*1024/(ordered.size()+1))return fail("composition work budget");
        for(size_t i=0;i<ordered.size();++i) {
            const auto& r=ordered[i];AssetCandidateBounds box;if(!assetCandidateBounds(r,box))return fail("resolved bounds");boxes.push_back(box);
            if(r.anchorVx==p.anchorVx&&r.anchorVy==p.anchorVy&&r.anchorVz==p.anchorVz&&r.yawQuarter==p.yawQuarter&&r.layer==p.layer&&r.bankId==p.bankId&&r.seedIndex==p.seedIndex) {
                if(selected!=ordered.size()||r.grid->sizeX()!=canonical.sizeX()||r.grid->sizeY()!=canonical.sizeY()||r.grid->sizeZ()!=canonical.sizeZ()||identity.contentHash(*r.grid)!=hash)return fail("canonical source changed");
                selected=i;
            }
        }
        if(selected==ordered.size())return fail("canonical source missing");
        const size_t count=size_t(nx*ny*nz);
        auto terrain=std::make_shared<std::vector<MaterialId>>(count,MAT_AIR);
        auto source=std::make_shared<std::vector<MaterialId>>(count,MAT_AIR);
        auto projection=std::make_shared<std::vector<MaterialId>>(count,MAT_AIR);
        for(int64_t y=domain.y0;y<=domain.y1;++y)for(int64_t x=domain.x0;x<=domain.x1;++x) {
            const auto col=world.amplifier().columnCached(x,y);
            for(int64_t z=domain.z0;z<=domain.z1;++z) {
                const size_t j=size_t(x-domain.x0)+size_t(nx)*(size_t(y-domain.y0)+size_t(ny)*size_t(z-domain.z0));
                auto m=Amplifier::materialAt(col,z);MaterialId owned=MAT_AIR;
                if(m==MAT_AIR) {
                    const auto i=assetAuthorityWinner(ordered,boxes,x,y,z);
                    if(i<ordered.size()) {
                        const auto a=assetCandidateMaterial(ordered[i],boxes[i],x,y,z);
                        if(i==selected)owned=a;else m=a;
                    }
                }
                (*terrain)[j]=m;(*source)[j]=owned;
                const auto px=x-b.x0,py=y-b.y0,pz=z-b.z0;
                if(px>=0&&py>=0&&pz>=0&&px<current.sizeX()&&py<current.sizeY()&&pz<current.sizeZ()) {
                    const auto a=current.at(int32_t(px),int32_t(py),int32_t(pz));
                    if(a!=MAT_AIR&&a!=owned)return fail("projection outside owned winner");
                    (*projection)[j]=a;
                }
            }
        }
        std::shared_ptr<StationaryAssetAuthority> out(new StationaryAssetAuthority);
        out->domain_=domain;out->nx_=size_t(nx);out->ny_=size_t(ny);out->provenance_=p;out->generation_=generation;out->revision_=objectRevision;
        out->terrain_=terrain;out->source_=source;out->projection_=projection;out->edits_=std::make_shared<std::vector<Override>>(count);return out;
    }
private:
    friend class StationaryAssetAuthorityCoordinator<B>;
    struct Override {bool present=false;MaterialId material=MAT_AIR;};
    StationaryAssetAuthority()=default;
    bool index(int64_t x,int64_t y,int64_t z,size_t& i) const {
        if(x<domain_.x0||x>domain_.x1||y<domain_.y0||y>domain_.y1||z<domain_.z0||z>domain_.z1)return false;
        i=size_t(x-domain_.x0)+nx_*(size_t(y-domain_.y0)+ny_*size_t(z-domain_.z0));return true;
    }
    std::shared_ptr<StationaryAssetAuthority> clone() const {
        auto lease=retention_?retention_->reserveSibling():nullptr;
        if(retention_&&!lease)return {};
        std::shared_ptr<StationaryAssetAuthority> out(new StationaryAssetAuthority);
        out->retention_=std::move(lease);
        out->domain_=domain_;out->nx_=nx_;out->ny_=ny_;out->provenance_=provenance_;out->generation_=generation_+1;out->revision_=revision_;
        out->terrain_=terrain_;out->source_=source_;out->projection_=projection_;out->edits_=edits_;return out;
    }
    AssetCandidateBounds domain_{};size_t nx_=0,ny_=0;AssetProvenance provenance_{};uint64_t generation_=0,revision_=0;
    std::shared_ptr<const IAssetAuthorityRetention> retention_;
    std::shared_ptr<const std::vector<MaterialId>> terrain_,source_,projection_;
    std::shared_ptr<const std::vector<Override>> edits_;
};
}
