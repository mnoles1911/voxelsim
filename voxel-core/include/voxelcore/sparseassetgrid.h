#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <span>
#include <stdexcept>
#include <tuple>

namespace vxc {
// Asset-local coordinates are [0,size); origin locates local zero relative to
// the asset anchor in source cells. Chunk keys NEVER contain origin offsets.
// A material byte of zero is air. Thread-confined: synchronize externally.
class SparseAssetGrid {
public:
    struct Shape { int32_t x=0,y=0,z=0; int64_t originX=0,originY=0,originZ=0; uint32_t pitchUm=0; };
    struct Limits { size_t residentChunks=131072, transactionChunks=4096; };
    struct Edit { int32_t x,y,z; uint8_t material; };
    struct Run { int32_t x,y,z,length; uint8_t material; };
    enum class Result { Ok, InvalidRange, BudgetExceeded, AllocationFailed };
    static constexpr size_t ChunkCells=512;
    explicit SparseAssetGrid(Shape shape):SparseAssetGrid(shape,Limits{}){}
    SparseAssetGrid(Shape shape, Limits limits):shape_(shape),limits_(limits) {
        const auto valid=[](int32_t n,int64_t o){return n>0 && o<=std::numeric_limits<int64_t>::max()-(n-1);};
        valid_=valid(shape.x,shape.originX)&&valid(shape.y,shape.originY)&&valid(shape.z,shape.originZ)&&shape.pitchUm;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        if (!valid_)
            throw std::invalid_argument("invalid sparse asset grid shape");
#endif
    }
    bool valid() const noexcept {return valid_;}
    const Shape& shape() const noexcept { return shape_; }
    size_t chunkCount() const noexcept { return chunks_.size(); }
    size_t residentPayloadBytes() const noexcept { return chunks_.size()*ChunkCells; }
    template<class Visitor> void visitChunks(Visitor&& visit) const {
        for(const auto& [k,c]:chunks_)visit(k.x,k.y,k.z,std::span<const uint8_t>(c));
    }
    // Erasing upper layers cannot allocate. Keep origin/shape stable for snapshots.
    void clearAbove(int32_t first) noexcept {
        for(auto it=chunks_.begin();it!=chunks_.end();){
            const int64_t base=int64_t(it->first.z)*8;
            if(base>=first){it=chunks_.erase(it);continue;}
            if(base+8>first)for(int64_t z=std::max<int64_t>(0,first-base);z<8;++z)
                std::fill_n(it->second.begin()+z*64,64,uint8_t(0));
            if(empty(it->second))it=chunks_.erase(it);else ++it;
        }
    }
    template<class Visitor> void visitOccupiedBox(int32_t x0,int32_t y0,int32_t z0,int32_t x1,int32_t y1,int32_t z1,Visitor&& visit) const {
        if(x0>=x1||y0>=y1||z0>=z1)return;
        x0=std::max(x0,0);y0=std::max(y0,0);z0=std::max(z0,0);
        x1=std::min(x1,shape_.x);y1=std::min(y1,shape_.y);z1=std::min(z1,shape_.z);
        if(x0>=x1||y0>=y1||z0>=z1)return;
        for(int32_t cx=x0/8;cx<=(x1-1)/8;++cx)for(int32_t cy=y0/8;cy<=(y1-1)/8;++cy){
            for(auto it=chunks_.lower_bound(Key{cx,cy,z0/8});it!=chunks_.end()&&it->first.x==cx&&it->first.y==cy&&int64_t(it->first.z)*8<z1;++it){
                for(int32_t z=std::max(z0,it->first.z*8);z<std::min(z1,it->first.z*8+8);++z)
                for(int32_t y=std::max(y0,cy*8);y<std::min(y1,cy*8+8);++y)
                for(int32_t x=std::max(x0,cx*8);x<std::min(x1,cx*8+8);++x){
                    const auto m=it->second[index(x,y,z)];if(m)visit(Edit{x,y,z,m});
                }
            }
        }
    }
    uint8_t at(int64_t x,int64_t y,int64_t z) const noexcept {
        if(!inside(x,y,z)) return 0;
        auto it=chunks_.find(key(x,y,z));
        return it==chunks_.end()?0:it->second[index(x,y,z)];
    }
    uint8_t atOriginRelative(int64_t x,int64_t y,int64_t z) const noexcept {
        if(!valid_)return 0;
        // Check the closed interval before subtraction, avoiding signed overflow.
        if(x<shape_.originX||x>shape_.originX+(shape_.x-1)||y<shape_.originY||y>shape_.originY+(shape_.y-1)||z<shape_.originZ||z>shape_.originZ+(shape_.z-1)) return 0;
        return at(x-shape_.originX,y-shape_.originY,z-shape_.originZ);
    }
    Result apply(std::span<const Edit> edits) {
        return transact([&](auto& staged){
            for(const auto& e:edits) {auto r=stageRun(staged,{e.x,e.y,e.z,1,e.material});if(r!=Result::Ok)return r;}
            return Result::Ok;
        });
    }
    Result applyRuns(std::span<const Run> runs) {
        return transact([&](auto& staged){
            for(const auto& run:runs){auto r=stageRun(staged,run);if(r!=Result::Ok)return r;}
            return Result::Ok;
        });
    }
    // Deterministic chunk-major (x,y,z), then cell-major (z,y,x) enumeration.
    // Visits only occupied cells; useful for mesh/query adapters without dense scans.
    template<class Visitor> void visitOccupiedCells(Visitor&& visit) const {
        for(const auto& [k,c]:chunks_)for(int z=0;z<8;++z)for(int y=0;y<8;++y)for(int x=0;x<8;++x){
            const auto m=c[size_t(x+8*y+64*z)];
            if(m)visit(Edit{k.x*8+x,k.y*8+y,k.z*8+z,m});
        }
    }
    // Emits only non-air runs, ordered by z, merging across chunk boundaries.
    // Runtime visits resident chunks in this XY chunk column, not logical height.
    // Callback must not mutate this grid; callback exceptions propagate.
    template<class Visitor> void visitColumnRuns(int32_t x,int32_t y,Visitor&& visit) const {
        if(!inside(x,y,0))return;
        Run pending{x,y,0,0,0};
        auto flush=[&](){if(pending.length){visit(pending);pending.length=0;}};
        for(auto it=chunks_.lower_bound(Key{x/8,y/8,0});it!=chunks_.end()&&it->first.x==x/8&&it->first.y==y/8;++it){
            const int64_t start=int64_t(it->first.z)*8;
            for(int64_t z=start;z<std::min(start+8,int64_t(shape_.z));++z){
                const uint8_t m=it->second[index(x,y,z)];
                if(!m){flush();continue;}
                if(pending.length&&pending.material==m&&int64_t(pending.z)+pending.length==z)++pending.length;
                else{flush();pending={x,y,int32_t(z),1,m};}
            }
        }
        flush();
    }
    // Environment object policy: any solid survives, air does not vote, ties
    // select lowest material ID. Deliberately distinct from terrain's threshold4.
    uint8_t environmentReducedAt2(int32_t x,int32_t y,int32_t z) const noexcept {
        return reducedAt2(x,y,z,1,false);
    }
    // Coarse coordinates relative to the asset anchor, including negative cells.
    // Unlike local reduction, this keeps odd source origins on the world lattice.
    uint8_t environmentReducedAtAnchor2(int64_t x,int64_t y,int64_t z) const noexcept {
        constexpr auto lo=std::numeric_limits<int64_t>::min()/2;
        constexpr auto hi=std::numeric_limits<int64_t>::max()/2;
        if(x<lo||x>hi||y<lo||y>hi||z<lo||z>hi)return 0;
        std::array<int,256> counts{};
        for(int dz=0;dz<2;++dz)for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){
            const auto m=atOriginRelative(x*2+dx,y*2+dy,z*2+dz);
            if(m)++counts[m];
        }
        int best=0;for(int m=1;m<256;++m)if(counts[m]>counts[best])best=m;
        return uint8_t(best);
    }
    // One exact 2x mip lookup; same threshold / lowest-ID tie / topmost policy
    // as downsampleBricks. Coordinates are coarse LOCAL cells; padding is air.
    uint8_t reducedAt2(int32_t x,int32_t y,int32_t z,int threshold=4,bool surfacePreserve=false) const noexcept {
        if(x<0||y<0||z<0||threshold<1||threshold>8)return 0;
        std::array<int,256> counts{};int solid=0,topZ=-1;uint8_t top=0;
        for(int dz=0;dz<2;++dz)for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){
            const auto m=at(int64_t(x)*2+dx,int64_t(y)*2+dy,int64_t(z)*2+dz);
            if(m){++solid;++counts[m];if(dz>topZ){topZ=dz;top=m;}}
        }
        if(solid<threshold)return 0;
        if(surfacePreserve)return top;
        int best=0;for(int m=1;m<256;++m)if(counts[m]>counts[best])best=m;
        return uint8_t(best);
    }
private:
    struct Key {int32_t x,y,z;bool operator<(const Key& b)const noexcept{return std::tie(x,y,z)<std::tie(b.x,b.y,b.z);}};
    using Chunk=std::array<uint8_t,ChunkCells>;
    using Chunks=std::map<Key,Chunk>;
    Shape shape_; Limits limits_; Chunks chunks_; bool valid_=false;
    bool inside(int64_t x,int64_t y,int64_t z) const noexcept{return valid_&&x>=0&&y>=0&&z>=0&&x<shape_.x&&y<shape_.y&&z<shape_.z;}
    static Key key(int64_t x,int64_t y,int64_t z) noexcept{return {int32_t(x/8),int32_t(y/8),int32_t(z/8)};}
    static size_t index(int64_t x,int64_t y,int64_t z) noexcept{return size_t((x%8)+8*(y%8)+64*(z%8));}
    static bool empty(const Chunk& c) noexcept{return std::all_of(c.begin(),c.end(),[](uint8_t m){return m==0;});}
    Result stageRun(Chunks& staged,const Run& r){
        if(r.length<=0||!inside(r.x,r.y,r.z)||int64_t(r.z)+r.length>shape_.z)return Result::InvalidRange;
        const int64_t end=int64_t(r.z)+r.length;
        for(int64_t z=r.z;z<end;){
            const auto k=key(r.x,r.y,z);auto it=staged.find(k);
            if(it==staged.end()){
                const auto old=chunks_.find(k);
                // Clearing absent chunks needs no allocation.
                if(old==chunks_.end()&&!r.material){z=std::min(end,(z/8+1)*8);continue;}
                if(staged.size()>=limits_.transactionChunks)return Result::BudgetExceeded;
                it=staged.emplace(k,old==chunks_.end()?Chunk{}:old->second).first;
            }
            const auto stop=std::min(end,(z/8+1)*8);
            for(;z<stop;++z)it->second[index(r.x,r.y,z)]=r.material;
        }
        return Result::Ok;
    }
    template<class Stage> Result transact(Stage&& stage){
        if(!valid_)return Result::InvalidRange;
        Chunks staged;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        try {const auto r=stage(staged);if(r!=Result::Ok)return r;}
        catch(const std::bad_alloc&){return Result::AllocationFailed;}
#else
        const auto r=stage(staged);if(r!=Result::Ok)return r;
#endif
        size_t count=chunks_.size();
        for(const auto& [k,c]:staged){if(chunks_.find(k)!=chunks_.end())--count;if(!empty(c))++count;}
        if(count>limits_.residentChunks)return Result::BudgetExceeded;
        // All allocation is complete. Equal standard allocators + noexcept key
        // comparison make map node transfer and fixed-array replacement nonthrowing.
        while(!staged.empty()){
            auto node=staged.extract(staged.begin());auto old=chunks_.find(node.key());
            if(empty(node.mapped())){if(old!=chunks_.end())chunks_.erase(old);}
            else if(old!=chunks_.end())old->second=node.mapped();
            else chunks_.insert(std::move(node));
        }
        return Result::Ok;
    }
};
} // namespace vxc
