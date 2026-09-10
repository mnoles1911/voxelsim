#pragma once
#include <array>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <deque>
#include <cstdint>
#include <memory>
#include "voxelcore/assetplacement.h"
namespace vxc {
using FootprintResolveKey=std::array<int32_t,3>; // level,X,Y: Z intentionally absent
// GT-only cache. Returned pointers last until the next mutating cache operation;
// existing CPU jobs copy values before dispatch. Grid owners remain host-owned.
template<class T> class FootprintResolveCache {
    std::shared_ptr<const char> owner_=std::make_shared<const char>(0);
    struct Entry {std::vector<T> value;typename std::list<FootprintResolveKey>::iterator lru;};
    std::map<FootprintResolveKey,Entry> entries_;
    std::list<FootprintResolveKey> lru_;
    std::set<FootprintResolveKey> history_;std::deque<FootprintResolveKey> historyOrder_;
    std::map<FootprintResolveKey,uint64_t> pending_;
    uint64_t bytes_=0,epoch_=1,serial_=0;size_t maxEntries_,maxHistory_,maxPending_;uint64_t maxBytes_,maxEntryBytes_;
    void evict(){auto k=lru_.back();auto it=entries_.find(k);bytes_-=it->second.value.capacity()*sizeof(T);entries_.erase(it);lru_.pop_back();++evictions;}
    void remember(const FootprintResolveKey& key){if(!maxHistory_||history_.contains(key))return;while(history_.size()>=maxHistory_){history_.erase(historyOrder_.front());historyOrder_.pop_front();++historyForgotten;}history_.insert(key);historyOrder_.push_back(key);}
public:
    struct Warm {FootprintResolveKey key{};uint64_t epoch=0,serial=0;std::weak_ptr<const char> owner;explicit operator bool()const{return serial!=0;}};
    uint64_t evictions=0,refused=0,stale=0,historyForgotten=0;
    FootprintResolveCache(size_t entries=8192,uint64_t bytes=64ull*1024*1024,uint64_t entryBytes=2ull*1024*1024,size_t history=8192,size_t pending=8)
        :maxEntries_(entries),maxHistory_(history),maxPending_(pending),maxBytes_(bytes),maxEntryBytes_(entryBytes){}
    FootprintResolveCache(const FootprintResolveCache&)=delete;FootprintResolveCache& operator=(const FootprintResolveCache&)=delete;
    void disable(){invalidate();epoch_=UINT64_MAX;}
    uint64_t epoch()const{return epoch_;}uint64_t bytes()const{return bytes_;}
    size_t size()const{return entries_.size();}size_t historySize()const{return history_.size();}size_t pendingSize()const{return pending_.size();}
    bool recentlyStored(const FootprintResolveKey& key)const{return history_.contains(key);}
    void invalidate(){entries_.clear();lru_.clear();bytes_=0;history_.clear();historyOrder_.clear();if(epoch_!=UINT64_MAX)++epoch_;}
    const std::vector<T>* find(const FootprintResolveKey& key){auto it=entries_.find(key);if(it==entries_.end()||epoch_==UINT64_MAX)return nullptr;lru_.splice(lru_.begin(),lru_,it->second.lru);return &it->second.value;}
    bool contains(const FootprintResolveKey& key)const{return entries_.contains(key);}
    bool putOwned(const FootprintResolveKey& key,std::vector<T>&& value,bool resident=true){
        if(!resident||!maxEntries_||epoch_==UINT64_MAX||value.capacity()>maxEntryBytes_/sizeof(T)||value.capacity()>maxBytes_/sizeof(T)){++refused;return false;}
        const uint64_t cost=value.capacity()*sizeof(T);
        if(auto it=entries_.find(key);it!=entries_.end()){bytes_-=it->second.value.capacity()*sizeof(T);lru_.erase(it->second.lru);entries_.erase(it);}
        while(!entries_.empty()&&(entries_.size()>=maxEntries_||cost>maxBytes_-bytes_))evict();
        lru_.push_front(key);entries_.emplace(key,Entry{std::move(value),lru_.begin()});bytes_+=cost;remember(key);return true;
    }
    bool put(const FootprintResolveKey& key,const std::vector<T>& value,bool resident=true){
        if(!resident||value.size()>maxEntryBytes_/sizeof(T)||value.size()>maxBytes_/sizeof(T)){++refused;return false;}
        return putOwned(key,std::vector<T>(value),resident);
    }
    Warm beginWarm(const FootprintResolveKey& key){
        if(epoch_==UINT64_MAX||serial_==UINT64_MAX||pending_.size()>=maxPending_||pending_.contains(key)||entries_.contains(key))return {};
        const auto serial=++serial_;pending_.emplace(key,serial);return {key,epoch_,serial,owner_};
    }
    // Stale work keeps its bounded pending slot until it actually lands. An old
    // completion cannot clear a newer same-key token or populate a new epoch.
    bool complete(const Warm& token,std::vector<T>&& value,bool resident){
        auto it=pending_.find(token.key);if(!token||token.owner.lock()!=owner_||it==pending_.end()||it->second!=token.serial){++stale;return false;}
        pending_.erase(it);
        if(token.epoch!=epoch_||!resident){++stale;return false;}
        if(entries_.contains(token.key))return true; // inline answer won the same epoch
        return putOwned(token.key,std::move(value),true);
    }
};
// Bound ALL site layers because AssetField::instancesForRect currently builds
// the all-layer site vector before terrainOnly filters column sampling.
inline bool footprintResolveSitesBound(const AssetVoxelRect& rect,const std::vector<AssetLayer>& layers,uint64_t cap=8192){
    if(!cap||cap>8192||rect.vx1<rect.vx0||rect.vy1<rect.vy0||layers.size()>kAssetLayerCount)return false;
    constexpr int64_t pitch=kVoxelSizeMm;
    constexpr int64_t safe=INT64_MAX/pitch-INT32_MAX;
    if(rect.vx0 < -safe||rect.vy0 < -safe||rect.vx1>safe||rect.vy1>safe)return false;
    const int64_t x0=rect.vx0*pitch,y0=rect.vy0*pitch,x1=rect.vx1*pitch+pitch-1,y1=rect.vy1*pitch+pitch-1;uint64_t total=0;
    for(const auto& l:layers){if(l.cellMm<=0||l.maxRadiusMm<0)return false;
        const int64_t r=l.maxRadiusMm;
        const auto nx=uint64_t(floorDiv(x1+r,int64_t(l.cellMm)))-uint64_t(floorDiv(x0-r,int64_t(l.cellMm)))+1;
        const auto ny=uint64_t(floorDiv(y1+r,int64_t(l.cellMm)))-uint64_t(floorDiv(y0-r,int64_t(l.cellMm)))+1;
        if(nx>cap||ny>cap||nx*ny>cap-total)return false;
        total+=nx*ny;
    }return true;
}
}
