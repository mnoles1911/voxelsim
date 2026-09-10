#pragma once
#include "voxelcore/world.h"
#include <limits>
#include <memory>

namespace vxc {
// Underground lighting/sky occlusion is a ground/rock question. Canopies,
// wood and the debug water marker must not turn an outdoor forest into a cave.
inline bool isUndergroundRoofMaterial(MaterialId material) {
    return material>=MAT_BEDROCK && material<=MAT_CLAY;
}
// Consumes the SAME already-rounded integer-mm O and D passed to raycastVoxels.
// Failure leaves an invalid rectangle, causing WorldQuery's exact fallback.
inline bool worldQueryRayRect(int64_t ox,int64_t oy,int64_t dx,int64_t dy,
                              AssetVoxelRect& out) {
    out={1,1,0,0};
    const auto axis=[](int64_t o,int64_t d,int64_t& lo,int64_t& hi){
        constexpr auto low=std::numeric_limits<int64_t>::min();
        constexpr auto high=std::numeric_limits<int64_t>::max();
        if((d>0 && o>high-d)||(d<0 && o<low-d))return false;
        const int64_t endMm=o+d;
        const int64_t first=floorDiv(o,kVoxelSizeMm);
        int64_t last=floorDiv(endMm,kVoxelSizeMm);
        // DDA visits t==1 and steps into the negative-side cell at a boundary.
        if(d<0 && endMm%kVoxelSizeMm==0)--last;
        lo=first<last?first:last;hi=first>last?first:last;
        constexpr int64_t limit=high/kVoxelSizeMm-INT32_MAX;
        return lo>=-limit && hi<=limit && hi-lo<=4096;
    };
    AssetVoxelRect result;
    if(!axis(ox,dx,result.vx0,result.vx1)||!axis(oy,dy,result.vy0,result.vy1))return false;
    out=result;return true;
}

// Short-lived, single-thread query. World, field, banks and channels must outlive
// it and retain their bring-up identity. Overlay reads remain live, not cached.
// Invalid/oversized rectangles and out-of-rectangle samples use exact World.
template<int B, int CraftRefinement = 3>
class WorldQuery {
public:
    WorldQuery(const World<B, CraftRefinement>& world, AssetVoxelRect rect)
        : world_(world), rect_(rect) {
        constexpr int64_t limit = std::numeric_limits<int64_t>::max()/kVoxelSizeMm - INT32_MAX;
        if (!rect.valid() || rect.vx0 < -limit || rect.vy0 < -limit ||
            rect.vx1 > limit || rect.vy1 > limit ||
            rect.vx1-rect.vx0 > 4096 || rect.vy1-rect.vy0 > 4096) return;
        const auto* field=world.assetField();
        if(field) for(const auto& layer:field->layers()) if(layer.maxRadiusMm<0) return;
        bounded_=true;
        if(!field || field->empty()) return;
        const auto instances=field->instancesForRect(rect,[&](int64_t x,int64_t y){
            // Copy before channel lookup: an external channel source may itself
            // consult the amplifier's thread-local column memo.
            const auto column=world.amplifier().columnCached(x,y);
            return assetColumnFactsFromSample(column,world.assetChannelsAt(x,y));
        },true); // Only terrain-lattice assets participate in World::materialAt.
        for(const auto& instance:instances) {
            auto resolved=field->resolveForCompose({instance});
            if(resolved.empty()) continue;
            entries_.push_back({instance.anchorXMm,instance.anchorYMm,
                field->layers()[instance.layer].maxRadiusMm,std::move(resolved)});
        }
    }
    bool usesShortlist() const {return bounded_;}
    size_t candidateCount() const {return entries_.size();}
    bool undergroundRoofAt(int64_t x,int64_t y,int64_t z) const {
        return isUndergroundRoofMaterial(materialAt(x,y,z));
    }
    MaterialId materialAt(int64_t x,int64_t y,int64_t z) const {
        if(!bounded_ || x<rect_.vx0 || x>rect_.vx1 || y<rect_.vy0 || y>rect_.vy1)
            return world_.materialAt(x,y,z);
        const auto key=ChunkMap<B>::keyForVoxel(x,y,z);
        if(const auto* brick=world_.editedBricks().find(key))
            return brick->get(int(floorMod(x,B)),int(floorMod(y,B)),int(floorMod(z,B)));
        const auto terrain=world_.amplifier().materialAt(x,y,z);
        if(terrain!=MAT_AIR) return terrain;
        const int64_t x0=x*kVoxelSizeMm,y0=y*kVoxelSizeMm;
        for(const auto& entry:entries_) {
            // Preserve the POINT query's reach filter, even for a malformed
            // bank whose geometry extends beyond its declared layer radius.
            if(entry.x+entry.radius<x0 || entry.x-entry.radius>x0+kVoxelSizeMm-1 ||
               entry.y+entry.radius<y0 || entry.y-entry.radius>y0+kVoxelSizeMm-1) continue;
            const auto material=AssetField::materialAtResolved(entry.resolved,x,y,z);
            if(material!=MAT_AIR) return material;
        }
        return MAT_AIR;
    }
private:
    struct Entry {
        int64_t x,y,radius;
        std::vector<AssetField::ResolvedAssetInstance> resolved;
    };
    const World<B,CraftRefinement>& world_;
    AssetVoxelRect rect_;
    bool bounded_=false;
    std::vector<Entry> entries_;
};
// One synchronous caller update only: bindings/channels must remain stable.
// Live overlay reads are deliberately not cached. References returned by
// prepare expire at the next prepare call which rebuilds the shortlist.
template<int B, int CraftRefinement = 3>
class WorldQueryBatch {
public:
    explicit WorldQueryBatch(const World<B,CraftRefinement>& world):world_(world) {}
    const WorldQuery<B,CraftRefinement>& prepare(AssetVoxelRect rect) {
        if(query_ && rect.valid() && rect.vx0>=covered_.vx0 && rect.vy0>=covered_.vy0 &&
            rect.vx1<=covered_.vx1 && rect.vy1<=covered_.vy1) return *query_;
        covered_=rect;
        constexpr int64_t margin=32;
        constexpr int64_t limit=std::numeric_limits<int64_t>::max()/kVoxelSizeMm-INT32_MAX;
        if(rect.valid() && rect.vx0>=-limit+margin && rect.vy0>=-limit+margin &&
            rect.vx1<=limit-margin && rect.vy1<=limit-margin &&
            rect.vx1-rect.vx0<=4096-2*margin && rect.vy1-rect.vy0<=4096-2*margin)
            covered_={rect.vx0-margin,rect.vy0-margin,rect.vx1+margin,rect.vy1+margin};
        query_=std::make_unique<WorldQuery<B,CraftRefinement>>(world_,covered_);
        ++preparations_;
        return *query_;
    }
    size_t preparationCount() const {return preparations_;}
private:
    const World<B,CraftRefinement>& world_;
    AssetVoxelRect covered_{1,1,0,0};
    std::unique_ptr<WorldQuery<B,CraftRefinement>> query_;
    size_t preparations_=0;
};
} // namespace vxc
