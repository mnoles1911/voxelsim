#pragma once
#include <algorithm>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace vxc {
// Stable source identity is separate from an actor address, bank cache slot or
// render chunk. Catalog/provider fingerprints deliberately prevent a saved
// edit from attaching to a different source after a world/catalog migration.
struct AssetProvenance {
    uint64_t worldSeed=0,providerFingerprint=0,catalogFingerprint=0;
    int64_t anchorVx=0,anchorVy=0,anchorVz=0;
    uint16_t bankId=0,seedIndex=0;
    uint8_t layer=0,yawQuarter=0;
    friend bool operator==(const AssetProvenance&,const AssetProvenance&)=default;
};
struct AssetObjectId {
    uint64_t low=0,high=0;
    friend bool operator==(const AssetObjectId&,const AssetObjectId&)=default;
    friend bool operator<(const AssetObjectId& a,const AssetObjectId& b) {
        return a.high<b.high||(a.high==b.high&&a.low<b.low);
    }
};
inline AssetObjectId assetObjectId(const AssetProvenance& p) {
    AssetObjectId id{14695981039346656037ull,7809847782465536322ull};
    auto word=[&](uint64_t n) {
        for(int i=0;i<8;++i){const uint8_t b=uint8_t(n>>(i*8));
            id.low=(id.low^b)*1099511628211ull;id.high=(id.high^b)*14029467366897019727ull;}
    };
    word(1);word(p.worldSeed);word(p.providerFingerprint);word(p.catalogFingerprint);
    word(uint64_t(p.anchorVx));word(uint64_t(p.anchorVy));word(uint64_t(p.anchorVz));
    word(p.bankId);word(p.seedIndex);word(p.layer);word(p.yawQuarter);
    return id;
}
enum class AssetRenderOwner:uint8_t { Terrain,Object };
enum AssetRenderBackend:uint8_t { AssetCpu=1,AssetGpu=2 };
struct AssetRenderPage {
    int64_t x=0,y=0,z=0;
    uint8_t level=0,backends=AssetCpu|AssetGpu;
    friend bool operator==(const AssetRenderPage&,const AssetRenderPage&)=default;
};
struct AssetOwnershipRecord {
    AssetProvenance provenance;
    AssetObjectId id;
    AssetRenderOwner owner=AssetRenderOwner::Terrain;
    uint64_t objectRevision=0,projectionRevision=0;
};
struct AssetOwnershipSnapshot {
    uint64_t generation=0;
    std::vector<AssetOwnershipRecord> records;
    bool objectOwns(const AssetProvenance& source) const {
        const auto id=assetObjectId(source);
        for(const auto& r:records)if(r.id==id)return r.provenance==source&&r.owner==AssetRenderOwner::Object;
        return false;
    }
};
struct AssetOwnershipTicket {
    uint64_t serial=0,sourceGeneration=0;
    friend bool operator==(const AssetOwnershipTicket&,const AssetOwnershipTicket&)=default;
};
// A publication transaction, not a material mask. The caller builds hidden
// object geometry and replacement terrain pages using target(). It must keep
// the old snapshot visible until publish invokes its all-or-nothing renderer
// callback. Collision/generated world sampling never reads this state.
class AssetRenderOwnership {
public:
    const AssetOwnershipSnapshot& visible() const {return visible_;}
    const AssetOwnershipSnapshot* target(AssetOwnershipTicket t) const {return matches(t)?&target_:nullptr;}
    bool acceptsVisibleJob(uint64_t generation) const {return generation==visible_.generation;}
    bool busy() const {return ticket_.serial!=0;}
    AssetOwnershipTicket begin(const AssetProvenance& source,AssetRenderOwner owner,
                               uint64_t objectRevision,uint64_t projectionRevision,
                               std::vector<AssetRenderPage> pages) {
        if(busy()||pages.empty()||pages.size()>8192||!objectRevision||projectionRevision!=objectRevision)return {};
        std::sort(pages.begin(),pages.end(),[](const auto& a,const auto& b){return std::tie(a.level,a.x,a.y,a.z)<std::tie(b.level,b.x,b.y,b.z);});
        for(size_t i=0;i<pages.size();++i){
            if(!pages[i].backends||(pages[i].backends&~uint8_t(AssetCpu|AssetGpu)))return {};
            if(i&&pages[i].x==pages[i-1].x&&pages[i].y==pages[i-1].y&&pages[i].z==pages[i-1].z&&pages[i].level==pages[i-1].level)return {};
        }
        const auto id=assetObjectId(source);target_=visible_;
        auto it=std::find_if(target_.records.begin(),target_.records.end(),[&](const auto& r){return r.id==id;});
        if(it!=target_.records.end()){
            if(!(it->provenance==source)||objectRevision<it->objectRevision)return {};
            it->owner=owner;it->objectRevision=objectRevision;it->projectionRevision=projectionRevision;
        }else target_.records.push_back({source,id,owner,objectRevision,projectionRevision});
        target_.generation=visible_.generation+1;ticket_={++serial_,visible_.generation};
        objectRevision_=objectRevision;
        pages_=std::move(pages);ready_.assign(pages_.size(),0);objectReady_=owner==AssetRenderOwner::Terrain;
        return ticket_;
    }
    bool objectReady(AssetOwnershipTicket t,uint64_t revision) {
        if(!matches(t)||target_.records.empty())return false;
        // The changed record is remembered independently of record order.
        if(revision!=objectRevision_)return false;
        objectReady_=true;return true;
    }
    bool pageReady(AssetOwnershipTicket t,const AssetRenderPage& page,uint8_t backend,uint64_t generation) {
        if(!matches(t)||generation!=target_.generation||(backend!=AssetCpu&&backend!=AssetGpu))return false;
        for(size_t i=0;i<pages_.size();++i)if(pages_[i].x==page.x&&pages_[i].y==page.y&&pages_[i].z==page.z&&pages_[i].level==page.level){
            if(!(pages_[i].backends&backend))return false;
            ready_[i]|=backend;return true;
        }
        return false;
    }
    bool ready(AssetOwnershipTicket t) const {
        if(!matches(t)||!objectReady_)return false;
        for(size_t i=0;i<pages_.size();++i)if(ready_[i]!=pages_[i].backends)return false;
        return true;
    }
    template<class AtomicPublish> bool publish(AssetOwnershipTicket t,AtomicPublish&& apply) {
        if(!ready(t))return false;
        // apply must enqueue every page replacement and object visibility in
        // one renderer transaction; false must leave the old scene untouched.
        if(!apply(std::as_const(visible_),std::as_const(target_),std::as_const(pages_)))return false;
        visible_=std::move(target_);clear();return true;
    }
    bool cancel(AssetOwnershipTicket t){if(!matches(t))return false;clear();return true;}
private:
    bool matches(AssetOwnershipTicket t) const {return t.serial&&t==ticket_&&t.sourceGeneration==visible_.generation;}
    void clear(){ticket_={};target_={};pages_.clear();ready_.clear();objectReady_=false;}
    AssetOwnershipSnapshot visible_,target_;
    AssetOwnershipTicket ticket_;
    uint64_t serial_=0,objectRevision_=0;
    std::vector<AssetRenderPage> pages_;
    std::vector<uint8_t> ready_;
    bool objectReady_=false;
};
// Render ownership must suppress the canonical winning cell, not remove an
// instance before composition: later overlapping assets would become visible.
// assetcandidate.h provides the cell-level CPU reference. Authoritative world
// and collision queries remain unfiltered.
}
