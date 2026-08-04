#pragma once
// Little-endian byte stream helpers, exact on all platforms regardless of
// host endianness. Shared by every wire format in voxel-core: the edit log
// (voxelcore/editlog.h) and the tile decoder (voxelcore/tilestore.h,
// terrain-service/terrain_service/tile_codec.py's C++ counterpart).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vxc {

class ByteWriter {
public:
    explicit ByteWriter(std::vector<uint8_t>& out) : out_(&out) {}
    void u8(uint8_t v) { out_->push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }

private:
    std::vector<uint8_t>* out_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    bool u8(uint8_t& v) {
        if (p_ == end_) return false;
        v = *p_++;
        return true;
    }
    bool u16(uint16_t& v) {
        uint8_t a, b;
        if (!u8(a) || !u8(b)) return false;
        v = static_cast<uint16_t>(a | (b << 8));
        return true;
    }
    bool u32(uint32_t& v) {
        uint16_t a, b;
        if (!u16(a) || !u16(b)) return false;
        v = uint32_t(a) | uint32_t(b) << 16;
        return true;
    }
    bool u64(uint64_t& v) {
        uint32_t a, b;
        if (!u32(a) || !u32(b)) return false;
        v = uint64_t(a) | uint64_t(b) << 32;
        return true;
    }
    bool i32(int32_t& v) {
        uint32_t u;
        if (!u32(u)) return false;
        v = static_cast<int32_t>(u);
        return true;
    }
    // Advances past `n` bytes without interpreting them. False (and NO advance)
    // if fewer than `n` remain, so a short buffer cannot be skipped past into
    // whatever follows it in memory.
    bool skip(size_t n) {
        if (static_cast<size_t>(end_ - p_) < n) return false;
        p_ += n;
        return true;
    }
    bool atEnd() const { return p_ == end_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

} // namespace vxc
