from pathlib import Path
r=Path('D:/voxelsim')
def edit(path, fn):
 p=r/path;s=p.read_text(encoding='utf-8');p.write_text(fn(s),encoding='utf-8')
def vxa(s):
 start=s.index('    # Millimetres, and an integer,')
 end=s.index('    header = (',start)
 s=s[:start]+'''    # v3 retains integer-mm bytes for existing assets. v4 stores pitch in
    # integer micrometres at the same offset; joints remain integer mm.
    voxel_um = grid.voxel_m * 1_000_000.0
    if not np.isfinite(voxel_um) or abs(voxel_um-round(voxel_um)) > 1e-6 or not 0 < voxel_um <= 4_096_000:
        raise ValueError("VXA pitch must be a positive integer micrometre value <= 4096 mm")
    voxel_um = int(round(voxel_um))
    version = 3 if voxel_um % 1000 == 0 else 4
    wire_pitch = voxel_um // 1000 if version == 3 else voxel_um
    voxel_mm = voxel_um / 1000.0

'''+s[end:]
 s=s.replace('struct.pack("<I", VERSION)','struct.pack("<I", version)').replace('int(round(voxel_mm))','wire_pitch').replace('mm = float(round(voxel_mm))','mm = float(voxel_mm)')
 s=s.replace('if version != VERSION:', 'if version not in (3, 4):')
 s=s.replace('    at = HEADER_BYTES\n', '    if version == 4:\n        voxel_mm /= 1000.0\n    if not 0 < voxel_mm <= 4096:\n        raise ValueError("invalid VXA voxel pitch")\n    at = HEADER_BYTES\n',1)
 return s
edit('asset-forge/forge/vxa.py',vxa)
edit('asset-forge/forge/spec.py',lambda s:s.replace('choices=("10", "5", "2.5", "2", "1")','choices=("10", "5", "2.5", "1.25", "2", "1")'))
edit('voxel-core/include/voxelcore/assetgrid.h',lambda s:s.replace('uint32_t voxelSizeMm() const { return voxelSizeMm_; }','uint32_t voxelSizeUm() const { return voxelSizeUm_; }\n    double voxelSizeMm() const { return double(voxelSizeUm_) / 1000.0; }').replace('voxelSizeMm_ == uint32_t(kVoxelSizeMm)','voxelSizeUm_ == uint32_t(kVoxelSizeMm) * 1000u').replace('uint32_t voxelSizeMm_ = 0','uint32_t voxelSizeUm_ = 0'))
edit('voxel-core/src/assetgrid.cpp',lambda s:s.replace('voxelSizeMm_ = 0','voxelSizeUm_ = 0').replace('if (readU32(blob + 4) != kVxaVersion)', 'const uint32_t version = readU32(blob + 4);\n    if (version != 3u && version != 4u)').replace('const uint32_t voxelMm = readU32(blob + 32);','const uint32_t wirePitch = readU32(blob + 32);\n    const uint64_t voxelUm = uint64_t(wirePitch) * (version == 3 ? 1000u : 1u);').replace('voxelMm == 0 || voxelMm > kMaxVoxelMm','voxelUm == 0 || voxelUm > uint64_t(kMaxVoxelMm) * 1000u').replace('voxelSizeMm_ = voxelMm','voxelSizeUm_ = uint32_t(voxelUm)'))
edit('ue-project/Source/VoxelEarth/VoxelAssetBody.cpp',lambda s:s.replace('pitch=%u','pitch=%.3f').replace('%u mm','%.3f mm'))

layout='''
// The 25 mm layout remains readable; new world edits use three binary folds.
inline constexpr uint32_t kFinestCraftPitchUm = 12500;
template<int Refinement>
struct CraftLayout {
    static_assert(Refinement == 2 || Refinement == 3, "supported craft pitches: 25 and 12.5 mm");
    static constexpr int kCraftCellsPerVoxel = 1 << Refinement;
    static constexpr uint32_t kCraftPitchUm = 100000u / kCraftCellsPerVoxel;
    static constexpr double kCraftPitchMm = double(kCraftPitchUm) / 1000.0;
    // Ownership region, deliberately distinct from a 32-cell render page.
    static constexpr int kCraftChunkEdgeCells = 8 * kCraftCellsPerVoxel;
    static constexpr int kCraftBricksPerAxis = kCraftCellsPerVoxel;
    static constexpr int kCraftBricksPerChunk = kCraftBricksPerAxis*kCraftBricksPerAxis*kCraftBricksPerAxis;
    static constexpr int kVoxelsPerCraftBrick = 8 / kCraftCellsPerVoxel;
    static constexpr int kCraftProjectionFolds = Refinement;
    static constexpr int kPagesPerAxis = kCraftChunkEdgeCells / kMarchChunkEdgeVoxels;
    static constexpr int kPageCount = kPagesPerAxis*kPagesPerAxis*kPagesPerAxis;
    static constexpr int64_t craftCellOfVoxelMin(int64_t v) { return v*kCraftCellsPerVoxel; }
    static constexpr int64_t voxelOfCraftCell(int64_t c) { return floorDiv(c,int64_t(kCraftCellsPerVoxel)); }
    static BrickKey craftBrickKeyOfCell(int64_t x,int64_t y,int64_t z) { return ChunkMap<8>::keyForVoxel(x,y,z); }
    static BrickKey craftChunkKeyOfCell(int64_t x,int64_t y,int64_t z) {
        return {int32_t(floorDiv(x,int64_t(kCraftChunkEdgeCells))),int32_t(floorDiv(y,int64_t(kCraftChunkEdgeCells))),int32_t(floorDiv(z,int64_t(kCraftChunkEdgeCells)))};
    }
    static BrickKey craftBrickBaseOfTerrainBrick(const BrickKey& k) {
        return {k.x*kCraftBricksPerAxis,k.y*kCraftBricksPerAxis,k.z*kCraftBricksPerAxis};
    }
    static BrickKey terrainBrickOfCraftBrick(const BrickKey& k) {
        return {int32_t(floorDiv(int64_t(k.x),int64_t(kCraftBricksPerAxis))),int32_t(floorDiv(int64_t(k.y),int64_t(kCraftBricksPerAxis))),int32_t(floorDiv(int64_t(k.z),int64_t(kCraftBricksPerAxis)))};
    }
};
'''
names=['kCraftCellsPerVoxel','kCraftChunkEdgeCells','kCraftBricksPerAxis','kCraftBricksPerChunk','kVoxelsPerCraftBrick','kCraftProjectionFolds','kCraftPitchUm','kCraftPitchMm']
funcs=['craftCellOfVoxelMin','voxelOfCraftCell','craftBrickKeyOfCell','craftChunkKeyOfCell','craftBrickBaseOfTerrainBrick','terrainBrickOfCraftBrick']
using='\n'.join('    using Layout::'+n+';' for n in names+funcs)
def lattice(s):
 s=s.replace('template <int B>\nclass CraftLattice {',layout+'\ntemplate <int B, int Refinement = 2>\nclass CraftLattice : public CraftLayout<Refinement> {\npublic:\n    using Layout = CraftLayout<Refinement>;\n'+using)
 a=s.index('        // Round 1: 64 craft bricks')
 b=s.index('        out.tryCollapse();',a)
 s=s[:a]+'''        int edge = kCraftBricksPerAxis;
        std::vector<CraftBrick> current;
        current.reserve(edge*edge*edge);
        for (int z=0;z<edge;++z) for(int y=0;y<edge;++y) for(int x=0;x<edge;++x) {
            const auto* brick=cells_.find({base.x+x,base.y+y,base.z+z});
            if (!brick) { ++counters.projectRefusedMissingBrick; return false; }
            current.push_back(*brick);
        }
        while(edge>1) {
            const int nextEdge=edge/2;
            std::vector<CraftBrick> next; next.reserve(nextEdge*nextEdge*nextEdge);
            for(int z=0;z<nextEdge;++z) for(int y=0;y<nextEdge;++y) for(int x=0;x<nextEdge;++x) {
                const CraftBrick* children[8];
                for(int dz=0;dz<2;++dz) for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx)
                    children[dx+2*dy+4*dz]=&current[(2*x+dx)+edge*((2*y+dy)+edge*(2*z+dz))];
                next.push_back(downsampleBricks<kMarchBrickEdge>(children));
            }
            current=std::move(next); edge=nextEdge;
        }
        out=std::move(current.front());
'''+s[b:]
 return s
edit('voxel-core/include/voxelcore/craftlattice.h',lattice)
def world(s):
 s=s.replace('template <int B>\nclass World {','template <int B, int CraftRefinement = 3>\nclass World : private CraftLayout<CraftRefinement> {\n    using Layout = CraftLayout<CraftRefinement>;\n'+using)
 s=s.replace('CraftLattice<B>','CraftLattice<B, CraftRefinement>')
 s=s.replace('static_cast<uint32_t>(kCraftPitchMm)','kCraftPitchMm')
 # Legacy cells expand exactly; loop order and append normalization remain deterministic.
 needle='        if (log.latticePitchMm() != kCraftPitchMm) return false;'
 repl='''        if constexpr (CraftRefinement == 3) {
            if (log.latticePitchMm() == 25.0) {
                EditLog expanded(log.seed(), log.brickEdge(), log.providerId(), kCraftPitchMm);
                for (const auto& entry: log.entries()) {
                    std::vector<std::pair<BrickKey,std::vector<EditCell>>> buckets;
                    for(const auto& cell: entry.cells) {
                        const int64_t x0=(int64_t(entry.key.x)*8+cell.cell%8)*2;
                        const int64_t y0=(int64_t(entry.key.y)*8+(cell.cell/8)%8)*2;
                        const int64_t z0=(int64_t(entry.key.z)*8+cell.cell/64)*2;
                        for(int dz=0;dz<2;++dz) for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx) {
                            const auto key=craftBrickKeyOfCell(x0+dx,y0+dy,z0+dz);
                            auto it=std::find_if(buckets.begin(),buckets.end(),[&](const auto& p){return p.first==key;});
                            if(it==buckets.end()) { buckets.push_back({key,{}}); it=std::prev(buckets.end()); }
                            it->second.push_back({uint16_t(Brick<8>::cellIndex(int(floorMod(x0+dx,int64_t(8))),int(floorMod(y0+dy,int64_t(8))),int(floorMod(z0+dz,int64_t(8))))),cell.mat});
                        }
                    }
                    std::sort(buckets.begin(),buckets.end(),[](const auto& a,const auto& b){return BrickKeyLess{}(a.first,b.first);});
                    for(auto& bucket:buckets) expanded.append(bucket.first,std::move(bucket.second));
                }
                return replayCraft(expanded);
            }
        }
'''+needle
 assert needle in s;s=s.replace(needle,repl)
 return s
edit('voxel-core/include/voxelcore/world.h',world)
# Existing legacy tests continue exercising the exact 25 mm behavior.
import re
edit('voxel-core/tests/test_craftpersist.cpp',lambda s:re.sub(r'World<([^>,]+)>',r'World<\1, 2>',s))

def logs(s):
 s=s.replace('kFormatVersion = 3','kFormatVersion = 4')
 s=s.replace('uint32_t latticePitchMm = static_cast<uint32_t>(kVoxelSizeMm))','double latticePitchMm = kVoxelSizeMm)')
 s=s.replace('latticePitchMm_(latticePitchMm)','latticePitchUm_(uint32_t(latticePitchMm * 1000.0))')
 s=s.replace('uint32_t latticePitchMm() const { return latticePitchMm_; }','uint32_t latticePitchUm() const { return latticePitchUm_; }\n    double latticePitchMm() const { return double(latticePitchUm_) / 1000.0; }')
 s=s.replace('return latticePitchMm_ == static_cast<uint32_t>(kVoxelSizeMm) ? 2u : 3u;','return latticePitchUm_ == uint32_t(kVoxelSizeMm)*1000u ? 2u : (latticePitchUm_ % 1000u == 0 ? 3u : 4u);')
 s=s.replace('if (fmt >= 3) w.u32(latticePitchMm_);','if (fmt >= 3) w.u32(fmt == 4 ? latticePitchUm_ : latticePitchUm_/1000u);')
 s=s.replace('std::move(providerId), pitchMm);','std::move(providerId), fmt == 4 ? double(pitchMm)/1000.0 : double(pitchMm));')
 s=s.replace('uint32_t latticePitchMm_ = static_cast<uint32_t>(kVoxelSizeMm);','uint32_t latticePitchUm_ = uint32_t(kVoxelSizeMm)*1000u;')
 return s
edit('voxel-core/include/voxelcore/editlog.h',logs)
print('Implemented binary layout, exact-pitch assets and versioned craft logs.')
