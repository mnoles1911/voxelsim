#pragma once
#include "voxelcore/assetauthority.h"
#include <mutex>
namespace vxc {
// Mutation methods are single-owner-thread operations. Immutable views may be
// read/destroyed on workers; ledger reserve/release is synchronized separately.
template<int B> class StationaryAssetAuthorityCoordinator final {
    using View=StationaryAssetAuthority<B>;
    struct Ledger;
    struct Credit final : IAssetAuthorityRetention {
        std::shared_ptr<Ledger> ledger;
        explicit Credit(std::shared_ptr<Ledger> l):ledger(std::move(l)){}
        ~Credit() override {std::lock_guard<std::mutex> guard(ledger->lock);--ledger->count;ledger->bytes-=creditBytes();}
        std::shared_ptr<const IAssetAuthorityRetention> reserveSibling() const override{return reserve(ledger);}
    };
    struct Ledger {
        std::mutex lock;uint64_t bytes=0,budget=0;size_t count=0,maxCount=0;bool closed=false;
    };
    static std::shared_ptr<const IAssetAuthorityRetention> reserve(const std::shared_ptr<Ledger>& l) {
        std::lock_guard<std::mutex> guard(l->lock);
        if(l->closed||l->count>=l->maxCount||creditBytes()>l->budget-l->bytes)return {};
        auto result=std::make_shared<Credit>(l);++l->count;l->bytes+=creditBytes();return result;
    }
public:
    using Ref=typename View::Ref;
    // Conservative full-generation charge: material arrays at maximum capture
    // cells plus known object/vector/ticket storage margin. Shared arrays are
    // intentionally double charged. Host callback caches and heap allocator
    // bookkeeping are not covered by this material-storage admission budget.
    static constexpr uint64_t creditBytes(){return 1024ull*1024*(3*sizeof(MaterialId)+sizeof(typename View::Override))+65536;}
    struct Usage {uint64_t bytes=0;size_t generations=0;};
    class Prepared {
        friend class StationaryAssetAuthorityCoordinator;
        std::weak_ptr<Ledger> owner;Ref base,candidate;uint64_t epoch=0;bool consumed=false;
    public:
        Prepared()=default;Prepared(const Prepared&)=delete;Prepared& operator=(const Prepared&)=delete;
    };
    using Ticket=std::shared_ptr<Prepared>;
    explicit StationaryAssetAuthorityCoordinator(uint64_t budget,size_t maxGenerations):ledger_(std::make_shared<Ledger>()) {
        ledger_->budget=budget;ledger_->maxCount=maxGenerations;
        if(budget>512ull*1024*1024||maxGenerations>64){ledger_->closed=true;ledger_->budget=0;ledger_->maxCount=0;}
    }
    ~StationaryAssetAuthorityCoordinator(){std::lock_guard<std::mutex> guard(ledger_->lock);ledger_->closed=true;}
    StationaryAssetAuthorityCoordinator(const StationaryAssetAuthorityCoordinator&)=delete;
    StationaryAssetAuthorityCoordinator& operator=(const StationaryAssetAuthorityCoordinator&)=delete;
    Ref visible() const{return visible_;}
    Usage usage() const{std::lock_guard<std::mutex> guard(ledger_->lock);return {ledger_->bytes,ledger_->count};}
    // Initial capture only. Reservation precedes all capture arrays and callbacks.
    // Args are exactly View::capture arguments; no arbitrary factory callback.
    template<class... Args> Ticket prepareInitial(Args&&... args) {
        if(visible_)return {};
        auto lease=reserve(ledger_);if(!lease)return {};
        auto captured=View::capture(std::forward<Args>(args)...);if(!captured)return {};
        // capture privately constructs a mutable object before returning const.
        std::const_pointer_cast<View>(captured)->retention_=std::move(lease);
        return ticket({},std::move(captured));
    }
    Ticket prepareCarve(const Ref& expected,int64_t x,int64_t y,int64_t z) {
        if(!expected||expected!=visible_)return {};
        return ticket(expected,expected->carve(expected->generation(),x,y,z));
    }
    Ticket prepareTerrainEditBatch(const Ref& expected,std::span<const AssetAuthorityTerrainEdit> cells) {
        if(!expected||expected!=visible_)return {};
        return ticket(expected,expected->editTerrainBatch(expected->generation(),cells));
    }
    Ticket prepareTerrainEdit(const Ref& expected,int64_t x,int64_t y,int64_t z,MaterialId m) {
        if(!expected||expected!=visible_)return {};
        return ticket(expected,expected->editTerrain(expected->generation(),x,y,z,m));
    }
    bool valid(const Ticket& t) const {
        if(!t||t->consumed||t->owner.lock()!=ledger_||t->epoch!=epoch_||t->base!=visible_||!t->candidate)return false;
        if(!t->base)return !visible_;
        return t->base->generation()!=UINT64_MAX&&t->candidate->generation()==t->base->generation()+1&&
            t->candidate->provenance()==t->base->provenance();
    }
    bool publish(const Ticket& t) {
        if(epoch_==UINT64_MAX||!valid(t))return false;
        visible_=std::move(t->candidate);++epoch_;t->base.reset();t->consumed=true;return true;
    }
    bool cancel(const Ticket& t) {
        if(!t||t->owner.lock()!=ledger_||t->consumed)return false;
        t->candidate.reset();t->base.reset();t->consumed=true;return true;
    }
private:
    Ticket ticket(Ref base,Ref candidate) {
        if(!candidate)return {};
        auto t=std::make_shared<Prepared>();t->owner=ledger_;t->base=std::move(base);t->candidate=std::move(candidate);t->epoch=epoch_;return t;
    }
    std::shared_ptr<Ledger> ledger_;Ref visible_;uint64_t epoch_=0;
};
}
