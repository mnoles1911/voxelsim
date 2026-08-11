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
    runMat_.clear();
    runLen_.clear();
    colRun_.clear();
    colOff_.clear();
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
    const uint32_t nruns = readU32(blob + 32);

    // Bound every extent so the cell count cannot overflow, and so a corrupt
    // header cannot ask for an allocation the size of the address space before
    // any other check runs. 4096 voxels is 409.6 m at 10 cm -- an order of
    // magnitude past the 28 m tallest asset, so this rejects corruption
    // without ever rejecting content.
    constexpr uint32_t kMaxEdge = 4096u;
    if (nx == 0 || ny == 0 || nz == 0) return AssetParseError::kBadDimensions;
    if (nx > kMaxEdge || ny > kMaxEdge || nz > kMaxEdge) return AssetParseError::kBadDimensions;

    const uint64_t cells = uint64_t(nx) * uint64_t(ny) * uint64_t(nz);
    const size_t bodyBytes = size_t(nruns) * kVxaRunBytes;
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
    return AssetParseError::kOk;
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

int32_t AssetGrid::rotatedOriginX(uint8_t yawQuarter) const {
    // Quarter turns about +z, right-handed: (x,y) -> (-y,x). The box's origin
    // corner moves with it, so the rotated origin is derived from the ORIGINAL
    // origin and the extent that ends up spanning the axis.
    switch (yawQuarter & 3u) {
        case 0: return originX_;
        case 1: return -(originY_ + sizeY_);
        case 2: return -(originX_ + sizeX_);
        default: return originY_;
    }
}

int32_t AssetGrid::rotatedOriginY(uint8_t yawQuarter) const {
    switch (yawQuarter & 3u) {
        case 0: return originY_;
        case 1: return originX_;
        case 2: return -(originY_ + sizeY_);
        default: return -(originX_ + sizeX_);
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
