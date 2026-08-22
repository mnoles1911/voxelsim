#pragma once
// Just enough HLSL to compile ue-project/Shaders/VoxelMaterialPalette.ush AS
// C++, so tools/check-palette-parity.py can run the SHIPPED shader text against
// vxc::voxelTint on a Linux box with no GPU and no DXC.
//
// WHY THIS EXISTS RATHER THAN A PYTHON MIRROR OF THE SHADER
// ---------------------------------------------------------
// The first version of the parity check re-implemented the shader's arithmetic
// in Python and compared that against the C++. It passed a deliberate
// corruption of the shader's hash constant and a deliberate reversion of its
// patch wavelength to the per-cube form -- because the Python was a copy of
// what the shader was SUPPOSED to say, so editing what it actually said changed
// nothing. It was a check of the checker.
//
// The shader text itself is the only thing worth comparing, so this compiles
// it. Three mechanical transforms are applied to the source first (the checker
// lists them and they are the whole delta):
//
//   [unroll]              deleted        an HLSL attribute with no C++ spelling
//   out T name            -> T& name     HLSL writes the direction before the type
//   .xyz                  -> .xyz_()     HLSL swizzles are members, C++ needs a call
//
// Nothing else is touched. If a future edit to the .ush needs a fourth
// transform, that is a signal worth reading -- the arithmetic is meant to stay
// simple enough to be checkable this way, and a construct that is not is
// probably a construct the definition in voxelcore/materialcolor.h does not
// have either.
//
// WHAT THIS IS NOT. It is not a shader compiler and it does not model HLSL's
// evaluation order, precision rules, or anything a GPU vendor might do
// differently. tools/compile-shaders.ps1 runs DXC over the same file, for both
// ADR-0001 targets, and that is the check for "will it compile and is it well
// defined". This one answers a different question: does it COMPUTE the same
// thing the engine header says.

#include <cmath>
#include <cstdint>

using uint = uint32_t;

struct float2 {
    float x = 0, y = 0;
    float2() = default;
    float2(float a, float b) : x(a), y(b) {}
};

struct float3 {
    float x = 0, y = 0, z = 0;
    float3() = default;
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
    // HLSL's colour spellings are the same lanes.
    float& r_() { return x; }
    // .r/.g/.b are handled by the members below rather than by aliases, because
    // C++ has no way to give one lane two names without a union, and a union of
    // a type with a constructor is more machinery than this needs. The shader
    // writes `c.r` and `c.b`; those two are provided as references into the
    // same object through the accessor macros at the bottom of this file.
};

struct float4 {
    float x = 0, y = 0, z = 0, w = 0;
    float4() = default;
    float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
    float4(float3 v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
    float3 xyz_() const { return float3(x, y, z); }
};

struct int3 {
    int32_t x = 0, y = 0, z = 0;
    int3() = default;
    int3(int32_t a, int32_t b, int32_t c) : x(a), y(b), z(c) {}
    explicit int3(float3 v)
        : x(int32_t(v.x)), y(int32_t(v.y)), z(int32_t(v.z)) {}
    // HLSL's (float3) cast on an int3.
    operator float3() const { return float3(float(x), float(y), float(z)); }
};

struct uint3 {
    uint x = 0, y = 0, z = 0;
};

inline uint3 asuint(int3 v) {
    uint3 u;
    u.x = uint(v.x);
    u.y = uint(v.y);
    u.z = uint(v.z);
    return u;
}

// --- float3 algebra, scalar on either side ---------------------------------
inline float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float3 operator*(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float3 operator+(float3 a, float s) { return {a.x + s, a.y + s, a.z + s}; }
inline float3 operator-(float s, float3 a) { return {s - a.x, s - a.y, s - a.z}; }
inline float3 operator*(float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float3 operator*(float s, float3 a) { return a * s; }
inline float3 operator/(float3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline int3 operator+(int3 a, int3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

// --- intrinsics -------------------------------------------------------------
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float3 floor(float3 v) { return {std::floor(v.x), std::floor(v.y), std::floor(v.z)}; }
inline float3 round(float3 v) { return {std::round(v.x), std::round(v.y), std::round(v.z)}; }
inline float3 max(float3 a, float s) { return {a.x > s ? a.x : s, a.y > s ? a.y : s, a.z > s ? a.z : s}; }
inline float3 min(float3 a, float s) { return {a.x < s ? a.x : s, a.y < s ? a.y : s, a.z < s ? a.z : s}; }
inline float3 max(float3 a, float3 b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z}; }
inline float3 min(float3 a, float3 b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z}; }
inline float3 saturate(float3 v) { return min(max(v, 0.0f), 1.0f); }
inline float saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float max(float a, float b) { return a > b ? a : b; }
inline float min(float a, float b) { return a < b ? a : b; }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline uint max(uint a, uint b) { return a > b ? a : b; }
inline float log2(float v) { return std::log2(v); }

// HLSL spells the red and blue lanes .r and .b. C++ cannot alias a member
// without a union, and a union of a type with a constructor costs more
// machinery than this needs, so the two places the shader uses them are
// rewritten by the same mechanical pass that handles [unroll] and out
// parameters. Keeping the substitution HERE, next to the type, is what stops it
// being invisible: `c.r *= k` becomes `c.x *= k`, which is the same lane.
#define VXC_HLSL_SHIM 1
