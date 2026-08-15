#include "voxelcore/assetgrid.h"

#include <cstring>

namespace vxc {

const char* assetParseErrorText(AssetParseError e) {
    switch (e) {
        case AssetParseError::kOk: return "ok";
        case AssetParseError::kTooSmall: return "blob smaller than a VXA1 header";
        case AssetParseError::kBadMagic: return "not a VXA1 file (bad magic)";
        case AssetParseError::kBadVersion: return "unsupported VXA version";
        case AssetParseError::kBadDimensions: return "zero or overflowing box dimensions";
        case AssetParseError::kTruncatedBody: return "run table truncated";
        case AssetParseError::kRunLengthSum: return "run lengths do not tile the box";
        case AssetParseError::kBadVoxelSize:
            return "voxel size of zero, or one no lattice can hold";
    }
    return "unknown";
}

namespace {

// Little-endian readers. Spelled out rather than memcpy'd into a uint32_t so
// the parser gives the same answer on a big-endian host; voxel-core has no
// big-endian target today, but a wire format read with host-endian punning is
// the kind of thing that is correct until the day it is silently not.
uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

int32_t readI32(const uint8_t* p) { return static_cast<int32_t>(readU32(p)); }

} // namespace

void AssetGrid::clear() {
    sizeX_ = sizeY_ = sizeZ_ = 0;
    originX_ = originY_ = originZ_ = 0;
    // Zero, not kVoxelSizeMm. A cleared grid must not answer `true` to
    // onTerrainLattice() -- that is the one question whose wrong answer stamps
    // a detail entity into the world.
    voxelSizeMm_ = 0;
    runMat_.clear();
    runLen_.clear();
    colRun_.clear();
    colOff_.clear();
    partMat_.clear();
    partLen_.clear();
    partColRun_.clear();
    partColOff_.clear();
    joints_.clear();
}

AssetParseError AssetGrid::parse(const uint8_t* blob, size_t bytes) {
    clear();
    if (blob == nullptr || bytes < kVxaHeaderBytes) return AssetParseError::kTooSmall;
    if (readU32(blob) != kVxaMagic) return AssetParseError::kBadMagic;
    if (readU32(blob + 4) != kVxaVersion) return AssetParseError::kBadVersion;

    const int32_t ox = readI32(blob + 8);
    const int32_t oy = readI32(blob + 12);
    const int32_t oz = readI32(blob + 16);
    const uint32_t nx = readU32(blob + 20);
    const uint32_t ny = readU32(blob + 24);
    const uint32_t nz = readU32(blob + 28);
    // Version 2 field. Everything after it shifted by four bytes, which is why
    // kVxaHeaderBytes moved 36 -> 40 and why a v1 blob is refused rather than
    // read: at these offsets a v1 file's run count would be read as its voxel
    // size and its first run record as its run count.
    const uint32_t voxelMm = readU32(blob + 32);
    const uint32_t nruns = readU32(blob + 36);
    const uint32_t npartRuns = readU32(blob + 40);
    const uint32_t njoints = readU32(blob + 44);

    // Bound every extent so the cell count cannot overflow, and so a corrupt
    // header cannot ask for an allocation the size of the address space before
    // any other check runs. 4096 voxels is 409.6 m at 10 cm -- an order of
    // magnitude past the 28 m tallest asset, so this rejects corruption
    // without ever rejecting content.
    constexpr uint32_t kMaxEdge = 4096u;
    if (nx == 0 || ny == 0 || nz == 0) return AssetParseError::kBadDimensions;
    if (nx > kMaxEdge || ny > kMaxEdge || nz > kMaxEdge) return AssetParseError::kBadDimensions;

    // A voxel has to have a size, and it has to be one an integer lattice can
    // express. The ceiling is generous on purpose: it rejects a corrupt or
    // uninitialised field without ever rejecting content, since the coarsest
    // thing anyone has baked is 100 mm.
    constexpr uint32_t kMaxVoxelMm = 4096u;
    if (voxelMm == 0 || voxelMm > kMaxVoxelMm) return AssetParseError::kBadVoxelSize;

    const uint64_t cells = uint64_t(nx) * uint64_t(ny) * uint64_t(nz);
    const size_t bodyBytes = size_t(nruns) * kVxaRunBytes
                           + size_t(npartRuns) * kVxaRunBytes
                           + size_t(njoints) * kVxaJointBytes;
    if (bytes < kVxaHeaderBytes + bodyBytes) return AssetParseError::kTruncatedBody;

    runMat_.resize(nruns);
    runLen_.resize(nruns);
    uint64_t total = 0;
    const uint8_t* rec = blob + kVxaHeaderBytes;
    for (uint32_t i = 0; i < nruns; ++i, rec += kVxaRunBytes) {
        runMat_[i] = static_cast<MaterialId>(rec[0]);
        const uint32_t len = readU32(rec + 1);
        runLen_[i] = len;
        total += len;
        // Checked inside the loop, not only after it: a corrupt header can
        // claim run lengths that sum past 2^64 across enough records, and a
        // sum that wrapped would compare equal to `cells` and pass.
        if (total > cells) return AssetParseError::kRunLengthSum;
    }
    if (total != cells) return AssetParseError::kRunLengthSum;

    sizeX_ = static_cast<int32_t>(nx);
    sizeY_ = static_cast<int32_t>(ny);
    sizeZ_ = static_cast<int32_t>(nz);
    voxelSizeMm_ = voxelMm;
    originX_ = ox;
    originY_ = oy;
    originZ_ = oz;

    // Column index. Iteration order is C order over [x][y][z], so the columns
    // themselves are contiguous and appear in (x major, y minor) order -- one
    // forward sweep over the runs assigns every column its entry point, and no
    // column ever needs a backward seek.
    const size_t columns = size_t(nx) * size_t(ny);
    colRun_.resize(columns);
    colOff_.resize(columns);
    {
        uint32_t run = 0;
        uint32_t off = 0; // cells of runLen_[run] already consumed
        for (size_t c = 0; c < columns; ++c) {
            // Skip any runs that are exhausted at this column boundary. A
            // zero-length run is not something asset-forge emits, but a
            // hand-built or future-tool blob could carry one and it must not
            // wedge the sweep.
            while (run < nruns && off >= runLen_[run]) {
                off = 0;
                ++run;
            }
            colRun_[c] = run;
            colOff_[c] = off;
            // Advance by one whole column.
            uint32_t remaining = nz;
            while (remaining > 0 && run < nruns) {
                const uint32_t avail = runLen_[run] - off;
                if (avail > remaining) {
                    off += remaining;
                    remaining = 0;
                } else {
                    remaining -= avail;
                    off = 0;
                    ++run;
                }
            }
        }
    }
    // Part runs, if any. Same tiling rule as the materials and the same column
    // sweep, deliberately duplicated rather than shared: the two tables differ
    // in element type and this is thirty lines, where a template over both
    // would put the hot `at` walk behind an indirection for no gain.
    const uint8_t* prec = blob + kVxaHeaderBytes + size_t(nruns) * kVxaRunBytes;
    if (npartRuns) {
        partMat_.resize(npartRuns);
        partLen_.resize(npartRuns);
        uint64_t ptotal = 0;
        const uint8_t* q = prec;
        for (uint32_t i = 0; i < npartRuns; ++i, q += kVxaRunBytes) {
            partMat_[i] = q[0];
            const uint32_t len = readU32(q + 1);
            partLen_[i] = len;
            ptotal += len;
            if (ptotal > cells) { clear(); return AssetParseError::kRunLengthSum; }
        }
        if (ptotal != cells) { clear(); return AssetParseError::kRunLengthSum; }

        partColRun_.resize(columns);
        partColOff_.resize(columns);
        uint32_t run = 0, off = 0;
        for (size_t c = 0; c < columns; ++c) {
            while (run < npartRuns && off >= partLen_[run]) { off = 0; ++run; }
            partColRun_[c] = run;
            partColOff_[c] = off;
            uint32_t remaining = nz;
            while (remaining > 0 && run < npartRuns) {
                const uint32_t avail = partLen_[run] - off;
                if (avail > remaining) { off += remaining; remaining = 0; }
                else { remaining -= avail; off = 0; ++run; }
            }
        }
    }

    const uint8_t* jrec = prec + size_t(npartRuns) * kVxaRunBytes;
    joints_.resize(njoints);
    for (uint32_t i = 0; i < njoints; ++i, jrec += kVxaJointBytes) {
        joints_[i].part = jrec[0];
        joints_[i].parent = jrec[1];
        joints_[i].xMm = readI32(jrec + 2);
        joints_[i].yMm = readI32(jrec + 6);
        joints_[i].zMm = readI32(jrec + 10);
    }
    return AssetParseError::kOk;
}

uint8_t AssetGrid::partAt(int32_t lx, int32_t ly, int32_t lz) const {
    if (partMat_.empty()) return 0;
    if (lx < 0 || ly < 0 || lz < 0 || lx >= sizeX_ || ly >= sizeY_ || lz >= sizeZ_) {
        return 0;
    }
    const size_t c = size_t(lx) * size_t(sizeY_) + size_t(ly);
    uint32_t run = partColRun_[c];
    uint32_t off = partColOff_[c];
    uint32_t want = static_cast<uint32_t>(lz);
    const uint32_t n = static_cast<uint32_t>(partLen_.size());
    while (run < n) {
        const uint32_t avail = partLen_[run] - off;
        if (want < avail) return partMat_[run];
        want -= avail;
        off = 0;
        ++run;
    }
    return 0;
}

MaterialId AssetGrid::at(int32_t lx, int32_t ly, int32_t lz) const {
    if (lx < 0 || ly < 0 || lz < 0 || lx >= sizeX_ || ly >= sizeY_ || lz >= sizeZ_) {
        return MAT_AIR;
    }
    const size_t c = size_t(lx) * size_t(sizeY_) + size_t(ly);
    uint32_t run = colRun_[c];
    uint32_t off = colOff_[c];
    uint32_t want = static_cast<uint32_t>(lz);
    const uint32_t nruns = static_cast<uint32_t>(runLen_.size());
    while (run < nruns) {
        const uint32_t avail = runLen_[run] - off;
        if (want < avail) return runMat_[run];
        want -= avail;
        off = 0;
        ++run;
    }
    return MAT_AIR;
}

// Quarter turns about +z, right-handed: (x,y) -> (-y,x). The box's origin
// corner moves with it, so the rotated origin is derived from the ORIGINAL
// origin and the extent that ends up spanning the axis.
//
// THE +1 ON EVERY NEGATED AXIS, AND WHY IT WAS MISSING. These are VOXEL INDICES,
// not a continuous interval, and negation flips which end of a half-open span is
// the closed one. An axis spanning indices [o, o+n) negates to the index set
// {-o, -o-1, ..., -(o+n-1)}, whose lowest member is -(o + n - 1) -- one MORE
// than the -(o + n) that comes from negating the exclusive upper bound. Four of
// the eight cases below negated an axis and all four were short by exactly that
// one.
//
// It was wrong from the day it was written and nothing said so, because
// NOTHING CALLED IT. assetgrid_yaw_is_a_bijection_that_preserves_content
// exercises atYaw thoroughly and never touches these two, and atYaw is
// self-consistent -- its coordinates are zero-based inside the rotated box, so
// it cannot notice an error in where that box sits in the world. The two halves
// only meet in a caller placing a yawed instance, and there was no such caller
// until assetfield.h. The symptom, when it finally appeared, was three of every
// four trees invisible: yaw 0 drew, and yaws 1-3 sampled one voxel outside the
// box on the negated axis and read the out-of-range MAT_AIR that at() answers
// by design. Not a crash, not a warning -- a thinner forest.
//
// assetgrid_rotated_origin_puts_a_yawed_box_where_atYaw_reads_it now pins the
// two against each other, which is the only test that could have caught this:
// either one alone is self-consistent.
int32_t AssetGrid::rotatedOriginX(uint8_t yawQuarter) const {
    switch (yawQuarter & 3u) {
        case 0: return originX_;
        case 1: return -(originY_ + sizeY_ - 1);
        case 2: return -(originX_ + sizeX_ - 1);
        default: return originY_;
    }
}

int32_t AssetGrid::rotatedOriginY(uint8_t yawQuarter) const {
    switch (yawQuarter & 3u) {
        case 0: return originY_;
        case 1: return originX_;
        case 2: return -(originY_ + sizeY_ - 1);
        default: return -(originX_ + sizeX_ - 1);
    }
}

MaterialId AssetGrid::atYaw(int32_t rx, int32_t ry, int32_t rz, uint8_t yawQuarter) const {
    // Inverse rotation from rotated-box local coords back to baked local
    // coords. Written as the inverse (not the forward map) because the caller
    // is a sampler: it holds the destination cell and needs the source.
    switch (yawQuarter & 3u) {
        case 0: return at(rx, ry, rz);
        case 1: return at(ry, sizeY_ - 1 - rx, rz);
        case 2: return at(sizeX_ - 1 - rx, sizeY_ - 1 - ry, rz);
        default: return at(sizeX_ - 1 - ry, rx, rz);
    }
}

uint64_t AssetGrid::solidCount() const {
    uint64_t n = 0;
    for (size_t i = 0; i < runMat_.size(); ++i) {
        if (runMat_[i] != MAT_AIR) n += runLen_[i];
    }
    return n;
}

MaterialId AssetGrid::maxMaterialId() const {
    MaterialId m = MAT_AIR;
    for (size_t i = 0; i < runMat_.size(); ++i) {
        if (runMat_[i] > m) m = runMat_[i];
    }
    return m;
}

size_t AssetGrid::footprintBytes() const {
    return runMat_.size() * sizeof(MaterialId) + runLen_.size() * sizeof(uint32_t) +
           colRun_.size() * sizeof(uint32_t) + colOff_.size() * sizeof(uint32_t);
}

} // namespace vxc
