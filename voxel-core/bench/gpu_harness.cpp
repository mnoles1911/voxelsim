// M0-gate Vulkan harness (ADR-0001): dispatches the SPIR-V worldgen kernels
// (voxel-core/shaders/worldgen.ush, ColumnMain + VoxelizeMain) on this
// machine's GPU and byte-compares every field/cell against the CPU reference
// (vxc::Amplifier::column / vxc::Amplifier::materialAt). This desktop is the
// ADR's AMD leg (RX 7800 XT) of the NVIDIA-vs-AMD M0 determinism gate.
//
// Bench code: wall-clock timing uses floats/doubles, but every value fed to
// or read from the shader is integer, matching docs/determinism.md and the
// float-free contract of voxel-core/src and voxel-core/include.
//
// No Vulkan SDK dependency: the Vulkan loader (vulkan-1.dll on Windows,
// libvulkan.so.1 on Linux) is loaded dynamically at runtime
// (LoadLibrary/dlopen + vkGetInstanceProcAddr) and every Vulkan entry point
// used below is resolved through that one function via the X-macro tables,
// so there is no import-lib link dependency — only tools/vulkan-headers'
// headers on Windows, or the system Vulkan headers on Linux
// (types/structs/PFN_ typedefs) are needed at compile time.
//
// Two kernels, two pipelines, one shared cbuffer, chained through one
// buffer: ColumnMain writes OutColumns; that SAME VkBuffer is then bound as
// VoxelizeMain's InColumns (a pipeline barrier makes the write visible to
// the second dispatch's reads) — columns are never recomputed on GPU.
//
// Resource bindings mirror worldgen.ush exactly, but DXC's default SPIR-V
// codegen maps each HLSL register class (b/t/u) independently onto the same
// flat Vulkan (set, binding) space, so b0/t0/u0 collide at (0,0) unless
// shifted. tools/compile-shaders.ps1 compiles with -fvk-t-shift 1 0
// -fvk-u-shift 3 0, producing the layout hardcoded here:
//   ColumnMain pipeline descriptor set:
//     binding 0: WorldGenParams   (VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b0)
//     binding 1: ElevationMm      (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, t0)
//     binding 2: ClimatePacked    (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, t1)
//     binding 3: OutColumns       (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, u0, RW)
//   VoxelizeMain pipeline descriptor set:
//     binding 0: WorldGenParams   (VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b0; same buffer as above)
//     binding 4: InColumns        (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, t3; same buffer as OutColumns)
//     binding 5: OutCells         (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, u2, RW)
//
// Platform split (M0 close-out, NVIDIA/Linux leg): everything below this
// comment is identical on both platforms except the ~30-line dynamic-loader
// shim directly below (LoadLibrary/GetProcAddress/FreeLibrary on Windows,
// dlopen/dlsym/dlclose on Linux) — the byte-compare logic, output format,
// and PASS/FAIL/digest reporting are untouched by platform.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

// Headless compute only — no WSI/surface, so plain vulkan_core.h (no
// platform header) is enough. VK_NO_PROTOTYPES keeps this a header-only
// dependency: no vulkan-1.lib/libvulkan.so link dependency, every entry
// point resolved dynamically below.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/core.h"
#include "voxelcore/mesher.h"
#include "voxelcore/tiles.h"

using namespace vxc;

namespace {

// --- Raster-window margin for the cavern pass (C6) -------------------------
// VoxelizeMain's cavernSiteFor evaluates the terrain surface at the SITE's own
// xy — caverns.h's `surfaceAt` contract, because caverns anchor at absolute z
// and a draped floor would be visibly wrong. A site can sit up to
// kCavernMaxReachMm away in x and y from the querying column
// (cavernColumnFromSites rejects anything further BEFORE it ever calls
// surfaceAt, so this bound is exact), the shader then floors that mm
// coordinate to a voxel (losing up to one more voxel) and takes bilinear taps
// at px and px+1.
//
// The uploaded raster window must therefore extend this far past the column
// range. Undersizing it does NOT fault: worldgen.ush's rasterElevationMm
// clamps to the window edge as a defensive backstop, so the GPU would read a
// different elevation than the CPU and the byte-compare would fail with no
// other symptom. That is exactly the failure mode this constant prevents.
constexpr int64_t kRasterCavernMarginMm = kCavernMaxReachMm + kVoxelSizeMm;

// --- Vulkan dynamic loading: no import lib, no static linkage -------------

#define VXC_VK_GLOBAL_FUNCS(X) X(vkCreateInstance)

#define VXC_VK_INSTANCE_FUNCS(X)          \
    X(vkDestroyInstance)                  \
    X(vkEnumeratePhysicalDevices)         \
    X(vkGetPhysicalDeviceProperties)      \
    X(vkGetPhysicalDeviceFeatures)        \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice)                     \
    X(vkGetDeviceProcAddr)                \
    X(vkDestroyDevice)

#define VXC_VK_DEVICE_FUNCS(X)            \
    X(vkGetDeviceQueue)                   \
    X(vkCreateBuffer)                     \
    X(vkDestroyBuffer)                    \
    X(vkGetBufferMemoryRequirements)      \
    X(vkAllocateMemory)                   \
    X(vkFreeMemory)                       \
    X(vkBindBufferMemory)                 \
    X(vkMapMemory)                        \
    X(vkUnmapMemory)                      \
    X(vkCreateShaderModule)               \
    X(vkDestroyShaderModule)              \
    X(vkCreateDescriptorSetLayout)        \
    X(vkDestroyDescriptorSetLayout)       \
    X(vkCreatePipelineLayout)             \
    X(vkDestroyPipelineLayout)            \
    X(vkCreateComputePipelines)           \
    X(vkDestroyPipeline)                  \
    X(vkCreateDescriptorPool)             \
    X(vkDestroyDescriptorPool)            \
    X(vkAllocateDescriptorSets)           \
    X(vkUpdateDescriptorSets)             \
    X(vkCreateCommandPool)                \
    X(vkDestroyCommandPool)               \
    X(vkAllocateCommandBuffers)           \
    X(vkBeginCommandBuffer)               \
    X(vkCmdBindPipeline)                  \
    X(vkCmdBindDescriptorSets)            \
    X(vkCmdDispatch)                      \
    X(vkCmdPipelineBarrier)               \
    X(vkEndCommandBuffer)                 \
    X(vkCreateFence)                      \
    X(vkDestroyFence)                     \
    X(vkWaitForFences)                    \
    X(vkQueueSubmit)                      \
    X(vkCreateQueryPool)                  \
    X(vkDestroyQueryPool)                 \
    X(vkCmdResetQueryPool)                \
    X(vkCmdWriteTimestamp)                \
    X(vkGetQueryPoolResults)

#define VXC_DECLARE_PFN(name) static PFN_##name name = nullptr;
VXC_VK_GLOBAL_FUNCS(VXC_DECLARE_PFN)
VXC_VK_INSTANCE_FUNCS(VXC_DECLARE_PFN)
VXC_VK_DEVICE_FUNCS(VXC_DECLARE_PFN)
#undef VXC_DECLARE_PFN

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

[[noreturn]] void fail(const std::string& msg) {
    std::fprintf(stderr, "FATAL: %s\n", msg.c_str());
    std::exit(1);
}

void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) fail(std::string(what) + " failed: VkResult " + std::to_string(r));
}

#ifdef _WIN32
using VulkanLoaderHandle = HMODULE;
#else
using VulkanLoaderHandle = void*;
#endif
VulkanLoaderHandle g_vulkanDll = nullptr;

void loadVulkanLoader() {
#ifdef _WIN32
    g_vulkanDll = LoadLibraryA("vulkan-1.dll");
    if (!g_vulkanDll) fail("vulkan-1.dll not found (no Vulkan runtime installed)");
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g_vulkanDll, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) fail("vulkan-1.dll missing vkGetInstanceProcAddr export");
#else
    // libvulkan.so.1 is the versioned SONAME every Vulkan loader package
    // (e.g. Ubuntu's libvulkan-dev / libvulkan1) installs; libvulkan.so
    // (unversioned, dev-only symlink) is a fallback for setups that only
    // installed the -dev package's symlink without the runtime package.
    g_vulkanDll = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!g_vulkanDll) g_vulkanDll = dlopen("libvulkan.so", RTLD_NOW);
    if (!g_vulkanDll)
        fail(std::string("libvulkan.so.1/libvulkan.so not found (no Vulkan runtime installed): ") +
             dlerror());
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(g_vulkanDll, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr)
        fail(std::string("libvulkan.so missing vkGetInstanceProcAddr export: ") + dlerror());
#endif

#define VXC_LOAD_GLOBAL(name)                                                          \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));        \
    if (!name) fail("missing global Vulkan entry point " #name);
    VXC_VK_GLOBAL_FUNCS(VXC_LOAD_GLOBAL)
#undef VXC_LOAD_GLOBAL
}

void closeVulkanLoader() {
#ifdef _WIN32
    FreeLibrary(g_vulkanDll);
#else
    dlclose(g_vulkanDll);
#endif
    g_vulkanDll = nullptr;
}

void loadInstanceFuncs(VkInstance instance) {
#define VXC_LOAD_INSTANCE(name)                                                        \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));       \
    if (!name) fail("missing instance-level Vulkan entry point " #name);
    VXC_VK_INSTANCE_FUNCS(VXC_LOAD_INSTANCE)
#undef VXC_LOAD_INSTANCE
}

void loadDeviceFuncs(VkDevice device) {
#define VXC_LOAD_DEVICE(name)                                                          \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));           \
    if (!name) fail("missing device-level Vulkan entry point " #name);
    VXC_VK_DEVICE_FUNCS(VXC_LOAD_DEVICE)
#undef VXC_LOAD_DEVICE
}

// --- misc helpers -----------------------------------------------------------

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) fail("cannot open shader binary: " + path);
    const std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(buf.data()), size))
        fail("failed reading shader binary: " + path);
    return buf;
}

uint32_t findMemoryType(const VkPhysicalDeviceMemoryProperties& memProps, uint32_t typeBits,
                         VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    fail("no Vulkan memory type with required properties " + std::to_string(required));
}

// GPU-side mirror of worldgen.ush's GpuColumnSample (5 x 4 bytes, tightly
// packed — verified against the compiled SPIR-V's member offsets).
struct GpuColumnSample {
    int32_t surfaceMm;
    int32_t topsoilMm;
    int32_t subsoilMm;
    int32_t bedrockDepthMm;
    uint32_t surfaceMat;
};
static_assert(sizeof(GpuColumnSample) == 20, "GpuColumnSample must match the HLSL layout");

// GPU-side mirror of worldgen.ush's cbuffer WorldGenParams (56 bytes,
// tightly packed — verified against the compiled SPIR-V's member offsets;
// nothing here straddles a 16-byte boundary so DXC's HLSL-style cbuffer
// packing matches plain sequential layout). BrickZMin/BricksZ are read only
// by VoxelizeMain; ScanCount only by Scan*Main; the rest is shared or
// ColumnMain-only (see worldgen.ush).
struct WorldGenParamsCB {
    uint32_t DispatchColumnsX, DispatchColumnsY;
    int32_t RasterOriginPxX, RasterOriginPxY;
    uint32_t RasterSizeX, RasterSizeY;
    int32_t PixelSizeMm;
    uint32_t SeedLo;
    uint32_t SeedHi;
    int32_t OriginVx;
    int32_t OriginVy;
    int32_t BrickZMin;
    uint32_t BricksZ;
    uint32_t ScanCount;
    // D5 coarse-level sampling: the coarse cell size in level-0 voxels, i.e.
    // 1 << level, computed HOST-side so worldgen.ush contains no variable shift
    // distance. ONE is level 0, which coarseRep() makes the exact identity, so
    // every bench fixture behaves as it did before this field existed -- and
    // the digest is what proves that rather than the claim.
    uint32_t CoarseScale;
    // D5.3 ring-boundary skirt; 0 for every bench fixture, and 0 makes the
    // whole block in regionCellMat dead, so the digest is unaffected.
    uint32_t RingSkirtMask;
};
static_assert(sizeof(WorldGenParamsCB) == 64, "WorldGenParamsCB must match the HLSL cbuffer layout");

// Host half of the 2026-07 cross-vendor UB hardening pass. worldgen.ush now
// guards these in-shader (it returns without writing rather than executing an
// OpUDiv-by-zero or an underflowed clamp bound), but a shader that silently
// declines to produce output is a miserable thing to debug — and the guards
// are a backstop, not a licence for the host to send garbage. Validating here
// means a bad cbuffer fails loudly, at the call site that built it, naming
// the field. Every value checked is a host-side invariant that already holds
// on every code path today; this only stops a future edit from breaking it
// quietly.
void validateWorldGenParams(const WorldGenParamsCB& p, const char* where) {
    // PixelSizeMm reaches floorDiv/truncDiv as a divisor in ColumnMain
    // (SPIR-V leaves OpUDiv-by-zero undefined, i.e. vendor-specific).
    if (p.PixelSizeMm == 0)
        fail(std::string(where) + ": PixelSizeMm is 0 — it is a divisor in ColumnMain");
    // RasterSize 0 underflows ColumnMain's clamp upper bound (RasterSize.x-1)
    // to -1, which the (uint) cast turns into a ~4-billion element index.
    if (p.RasterSizeX == 0 || p.RasterSizeY == 0)
        fail(std::string(where) + ": RasterSize has a zero extent (" +
             std::to_string(p.RasterSizeX) + "x" + std::to_string(p.RasterSizeY) + ")");
    // CoarseScale is a MULTIPLIER on the column coordinate (coarseRep), not a
    // level. Zero -- which is what a value-initialised WorldGenParamsCB leaves
    // here -- makes every column in the dispatch evaluate at world origin
    // (0,0), and it does so SILENTLY: rasterElevationMm clamps to each region's
    // own window, so the resulting terrain still varies from region to region
    // and looks plausible in a digest. This gate shipped in that state, so the
    // check is not hypothetical.
    if (p.CoarseScale == 0)
        fail(std::string(where) + ": CoarseScale is 0 - it multiplies the column coordinate in "
                                  "coarseRep, so 0 collapses every column to world origin. "
                                  "Level 0 is CoarseScale = 1, not 0.");
    // VoxelizeMain's brick indexing assumes a brick-aligned footprint.
    if (p.DispatchColumnsX % 8 != 0 || p.DispatchColumnsY % 8 != 0)
        fail(std::string(where) + ": DispatchColumns must be brick-aligned (multiples of 8), got " +
             std::to_string(p.DispatchColumnsX) + "x" + std::to_string(p.DispatchColumnsY));
}

// Checked immediately before the mesh chain is recorded. MeshEmitMain reads
// InQuadOffsets[tid.x] for every mask decodeMask accepts (tid.x < maskCount),
// and only entries below ScanCount were actually written by the scan chain —
// so ScanCount < maskCount would emit from never-written device memory.
// ScanCount > maskCount is harmless for the offsets of real masks but means
// the scan is reading past the counts buffer, which is equally not intended.
// The shader now refuses to emit above ScanCount; this asserts the exact
// equality the design actually relies on.
void validateScanCount(uint32_t scanCount, uint32_t maskCount, const char* where) {
    if (scanCount != maskCount)
        fail(std::string(where) + ": ScanCount (" + std::to_string(scanCount) +
             ") must equal maskCount (" + std::to_string(maskCount) +
             ") — MeshEmitMain reads one offset per mask");
}

// Cell index within one 8^3 brick — mirrors vxc::Brick<8>::cellIndex AND
// worldgen.ush's cellIndexInBrick exactly.
constexpr uint32_t cellIndexInBrick(uint32_t x, uint32_t y, uint32_t z) {
    return x + 8u * (y + 8u * z);
}

// OutCells (RWStructuredBuffer<uint>, one uint per cell, material id 0-255 in
// the low byte) layout — mirrors worldgen.ush's VoxelizeMain doc comment
// exactly:
//   bricksX = width / 8, bricksY = height / 8 (dispatch footprint, brick-aligned)
//   bx = x / 8, by = y / 8, lx = x % 8, ly = y % 8
//   footprintIndex = bx + bricksX * by
//   brickIndex(footprintIndex, bzLocal) = footprintIndex * bricksZ + bzLocal
//     (bzLocal in [0, bricksZ); actual brick z = brickZMin + bzLocal)
//   cell offset = brickIndex * 512 + cellIndexInBrick(lx, ly, zLocal)

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

// Sub-timers for the buffer-(re)allocation bracket. The single aggregate
// "buffer (re)allocation" number this harness used to print was labelled
// "vkAllocateMemory + vkMapMemory on grow", but GrowBuffer::ensure() starts
// its clock BEFORE destroying the outgoing buffer -- so the bracket also
// contained vkUnmapMemory + vkDestroyBuffer + vkFreeMemory of the OLD
// allocation, plus vkCreateBuffer and vkBindBufferMemory of the new one.
// That mislabel is exactly the kind of thing that has already produced one
// confidently-wrong attribution in this file's history (the "342 ms
// marshalling" number that turned out to be 0.129 ms of marshalling plus a
// buffer-grow conflated into the same bracket), so the components are now
// measured separately and printed individually. Pure instrumentation: no
// allocation sizes, buffer contents, or dispatch bounds change, so this
// cannot move a digest.
struct AllocProfile {
    double destroyOldMs = 0.0;   // vkUnmapMemory + vkDestroyBuffer + vkFreeMemory (old buffer)
    double createBufferMs = 0.0; // vkCreateBuffer + vkGetBufferMemoryRequirements
    double allocateMemoryMs = 0.0;
    double bindMs = 0.0;
    double mapMs = 0.0;
    uint64_t bytesAllocated = 0;
    size_t allocCount = 0;
    // Which host-visible memory type the allocations actually land in. On AMD
    // with resizable BAR the first HOST_VISIBLE|HOST_COHERENT type can also be
    // DEVICE_LOCAL (i.e. BAR-mapped VRAM), whose vkAllocateMemory cost is
    // sensitive to VRAM pressure from other processes -- worth printing, since
    // it makes an environment-dependent allocation stall diagnosable instead of
    // re-litigable.
    uint32_t memTypeIndex = UINT32_MAX;
    VkMemoryPropertyFlags memTypeFlags = 0;
    VkDeviceSize memHeapSize = 0;

    double totalMs() const {
        return destroyOldMs + createBufferMs + allocateMemoryMs + bindMs + mapMs;
    }
};

// --- the Vulkan device we run everything on ---------------------------------

struct GpuContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;

    // VoxelizeMain — separate pipeline/layout/module/set (different SPIR-V
    // module, different bindings), same instance/device/queue/commandPool.
    VkDescriptorSetLayout voxDescSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout voxPipelineLayout = VK_NULL_HANDLE;
    VkShaderModule voxShaderModule = VK_NULL_HANDLE;
    VkPipeline voxPipeline = VK_NULL_HANDLE;
    VkDescriptorPool voxDescPool = VK_NULL_HANDLE;
    VkDescriptorSet voxDescSet = VK_NULL_HANDLE;

    // Mesh + GPU-scan kernels (MeshCountMain / MeshEmitMain / ScanBlocksMain /
    // ScanSumsMain / ScanAddMain) share ONE superset descriptor layout
    // (binding 0 uniform; 5 cells, 6 offsets-read, 7 counts, 8 quads, 9
    // offsets-write, 10 block sums storage) — a layout binding a kernel's
    // SPIR-V doesn't declare is legal, and it lets every kernel in the chain
    // reuse identical set wiring. Bindings 9/10 are the scan extension: 9 is
    // the SAME VkBuffer as binding 6 (offsets), bound both read-only (t5, for
    // MeshEmitMain) and read-write (u6, for the scan kernels) at once.
    VkDescriptorSetLayout meshDescSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout meshPipelineLayout = VK_NULL_HANDLE;
    VkShaderModule meshCountModule = VK_NULL_HANDLE;
    VkShaderModule meshEmitModule = VK_NULL_HANDLE;
    VkShaderModule scanBlocksModule = VK_NULL_HANDLE;
    VkShaderModule scanSumsModule = VK_NULL_HANDLE;
    VkShaderModule scanAddModule = VK_NULL_HANDLE;
    VkPipeline meshCountPipeline = VK_NULL_HANDLE;
    VkPipeline meshEmitPipeline = VK_NULL_HANDLE;
    VkPipeline scanBlocksPipeline = VK_NULL_HANDLE;
    VkPipeline scanSumsPipeline = VK_NULL_HANDLE;
    VkPipeline scanAddPipeline = VK_NULL_HANDLE;
    VkDescriptorPool meshDescPool = VK_NULL_HANDLE;
    VkDescriptorSet meshCountSet = VK_NULL_HANDLE;
    VkDescriptorSet meshEmitSet = VK_NULL_HANDLE;
    VkDescriptorSet scanSet = VK_NULL_HANDLE; // bound for ScanBlocks/ScanSums/ScanAddMain

    // GPU timestamp queries: 6 slots bracket the 5 chained mesh/scan
    // dispatches (count, scanBlocks, scanSums, scanAdd, emit) inside the
    // SINGLE command buffer runMeshChain() records, so per-stage GPU time is
    // still measurable even though the chain only has one fence.
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    double timestampPeriodNs = 1.0;

    std::string deviceName;

    // Component breakdown of every createBuffer/destroyBuffer this context
    // performs. Snapshot it before teardown to get the in-gate figure (the
    // final cleanup's destroyBuffer calls also accumulate into destroyOldMs).
    AllocProfile allocProfile;

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b;
        b.size = size;
        const auto tCreate = Clock::now();
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(device, &bci, nullptr, &b.buffer), "vkCreateBuffer");

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        const VkMemoryPropertyFlags hostVisible =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(memProps, req.memoryTypeBits, hostVisible);
        allocProfile.createBufferMs += msSince(tCreate);

        const auto tAlloc = Clock::now();
        vkCheck(vkAllocateMemory(device, &mai, nullptr, &b.memory), "vkAllocateMemory");
        allocProfile.allocateMemoryMs += msSince(tAlloc);

        const auto tBind = Clock::now();
        vkCheck(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
        allocProfile.bindMs += msSince(tBind);

        const auto tMap = Clock::now();
        vkCheck(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped), "vkMapMemory");
        allocProfile.mapMs += msSince(tMap);

        allocProfile.bytesAllocated += uint64_t(req.size);
        ++allocProfile.allocCount;
        allocProfile.memTypeIndex = mai.memoryTypeIndex;
        allocProfile.memTypeFlags = memProps.memoryTypes[mai.memoryTypeIndex].propertyFlags;
        allocProfile.memHeapSize =
            memProps.memoryHeaps[memProps.memoryTypes[mai.memoryTypeIndex].heapIndex].size;
        return b;
    }

    void destroyBuffer(Buffer& b) {
        const auto t0 = Clock::now();
        if (b.mapped) vkUnmapMemory(device, b.memory);
        if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device, b.memory, nullptr);
        b = Buffer{};
        allocProfile.destroyOldMs += msSince(t0);
    }
};

GpuContext createContext(const std::string& spvPath, const std::string& voxelizeSpvPath,
                          const std::string& meshCountSpvPath, const std::string& meshEmitSpvPath,
                          const std::string& scanBlocksSpvPath,
                          const std::string& scanSumsSpvPath,
                          const std::string& scanAddSpvPath) {
    GpuContext ctx;

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "vxc_gpu";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "voxel-core";
    appInfo.engineVersion = 1;
    // worldgen.ColumnMain.spv is compiled with -fspv-target-env=vulkan1.1.
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;
    vkCheck(vkCreateInstance(&ici, nullptr, &ctx.instance), "vkCreateInstance");
    loadInstanceFuncs(ctx.instance);

    uint32_t deviceCount = 0;
    vkCheck(vkEnumeratePhysicalDevices(ctx.instance, &deviceCount, nullptr),
            "vkEnumeratePhysicalDevices(count)");
    if (deviceCount == 0) fail("no Vulkan physical devices found");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkCheck(vkEnumeratePhysicalDevices(ctx.instance, &deviceCount, devices.data()),
            "vkEnumeratePhysicalDevices");

    // Prefer the first DISCRETE_GPU; fall back to the first device of any type.
    VkPhysicalDevice fallback = devices[0];
    VkPhysicalDeviceProperties fallbackProps{};
    vkGetPhysicalDeviceProperties(fallback, &fallbackProps);
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties chosenProps{};
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            chosen = pd;
            chosenProps = props;
            break;
        }
    }
    if (chosen == VK_NULL_HANDLE) {
        chosen = fallback;
        chosenProps = fallbackProps;
    }
    ctx.physicalDevice = chosen;
    ctx.deviceName = chosenProps.deviceName;
    vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &ctx.memProps);

    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(ctx.physicalDevice, &features);
    if (!features.shaderInt64) {
        fail("chosen GPU (" + ctx.deviceName +
             ") does not support shaderInt64 — required by worldgen.ush's 64-bit hash "
             "(ADR-0001)");
    }

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfProps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &qfCount, qfProps.data());
    bool found = false;
    for (uint32_t i = 0; i < qfCount; ++i) {
        if (qfProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            ctx.queueFamily = i;
            found = true;
            break;
        }
    }
    if (!found) fail("chosen GPU (" + ctx.deviceName + ") has no compute-capable queue family");
    if (qfProps[ctx.queueFamily].timestampValidBits == 0) {
        fail("chosen GPU (" + ctx.deviceName +
             ")'s compute queue family reports 0 timestampValidBits — required for the "
             "per-stage GPU timing runMeshChain() uses to bracket the chained mesh/scan "
             "dispatches");
    }
    ctx.timestampPeriodNs = double(chosenProps.limits.timestampPeriod);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = ctx.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.shaderInt64 = VK_TRUE;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &enabledFeatures;
    vkCheck(vkCreateDevice(ctx.physicalDevice, &dci, nullptr, &ctx.device), "vkCreateDevice");
    loadDeviceFuncs(ctx.device);
    vkGetDeviceQueue(ctx.device, ctx.queueFamily, 0, &ctx.queue);

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = ctx.queueFamily;
    vkCheck(vkCreateCommandPool(ctx.device, &cpci, nullptr, &ctx.commandPool),
            "vkCreateCommandPool");

    // 6 timestamp slots bracket runMeshChain()'s 5 chained dispatches (see
    // GpuContext comment above).
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 6;
    vkCheck(vkCreateQueryPool(ctx.device, &qpci, nullptr, &ctx.timestampPool),
            "vkCreateQueryPool(timestamp)");

    // Descriptor set layout: binding 0 uniform (WorldGenParams), bindings
    // 1-2 read-only storage (ElevationMm, ClimatePacked), binding 3
    // read-write storage (OutColumns). See the file header for the DXC
    // -fvk-*-shift mapping that produces these binding numbers.
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 4;
    dslci.pBindings = bindings;
    vkCheck(vkCreateDescriptorSetLayout(ctx.device, &dslci, nullptr, &ctx.descSetLayout),
            "vkCreateDescriptorSetLayout");

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &ctx.descSetLayout;
    vkCheck(vkCreatePipelineLayout(ctx.device, &plci, nullptr, &ctx.pipelineLayout),
            "vkCreatePipelineLayout");

    const std::vector<uint8_t> spv = readFile(spvPath);
    if (spv.size() % 4 != 0) fail("SPIR-V binary size not a multiple of 4: " + spvPath);
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spv.size();
    smci.pCode = reinterpret_cast<const uint32_t*>(spv.data());
    vkCheck(vkCreateShaderModule(ctx.device, &smci, nullptr, &ctx.shaderModule),
            "vkCreateShaderModule");

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = ctx.shaderModule;
    stage.pName = "ColumnMain";
    VkComputePipelineCreateInfo cpci2{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci2.stage = stage;
    cpci2.layout = ctx.pipelineLayout;
    vkCheck(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &cpci2, nullptr,
                                      &ctx.pipeline),
            "vkCreateComputePipelines");

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = poolSizes;
    vkCheck(vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &ctx.descPool),
            "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = ctx.descPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &ctx.descSetLayout;
    vkCheck(vkAllocateDescriptorSets(ctx.device, &dsai, &ctx.descSet),
            "vkAllocateDescriptorSets");

    // --- VoxelizeMain pipeline: separate SPIR-V module, separate descriptor
    // set layout (binding 0 uniform WorldGenParams shared with ColumnMain's
    // buffer, binding 1 ElevationMm read-only storage, binding 4 InColumns
    // read-only storage, binding 5 OutCells RW storage — see the file header
    // for the DXC -fvk-*-shift derivation).
    //
    // Binding 1 is new as of the C6 cavern mirror. VoxelizeMain used to need
    // no raster at all (the cave pass is a pure function of seed/vx/vy/
    // surfaceMm), but caverns.h's `surfaceAt` contract makes the cavern pass
    // evaluate terrain height at the SITE's own xy — which is generally not a
    // column in the dispatch, so it cannot be carried on GpuColumnSample and
    // must be recomputed from the elevation raster in-shader.
    VkDescriptorSetLayoutBinding voxBindings[4]{};
    voxBindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    voxBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    voxBindings[2] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    voxBindings[3] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo voxDslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    voxDslci.bindingCount = 4;
    voxDslci.pBindings = voxBindings;
    vkCheck(vkCreateDescriptorSetLayout(ctx.device, &voxDslci, nullptr, &ctx.voxDescSetLayout),
            "vkCreateDescriptorSetLayout(voxelize)");

    VkPipelineLayoutCreateInfo voxPlci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    voxPlci.setLayoutCount = 1;
    voxPlci.pSetLayouts = &ctx.voxDescSetLayout;
    vkCheck(vkCreatePipelineLayout(ctx.device, &voxPlci, nullptr, &ctx.voxPipelineLayout),
            "vkCreatePipelineLayout(voxelize)");

    const std::vector<uint8_t> voxSpv = readFile(voxelizeSpvPath);
    if (voxSpv.size() % 4 != 0)
        fail("SPIR-V binary size not a multiple of 4: " + voxelizeSpvPath);
    VkShaderModuleCreateInfo voxSmci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    voxSmci.codeSize = voxSpv.size();
    voxSmci.pCode = reinterpret_cast<const uint32_t*>(voxSpv.data());
    vkCheck(vkCreateShaderModule(ctx.device, &voxSmci, nullptr, &ctx.voxShaderModule),
            "vkCreateShaderModule(voxelize)");

    VkPipelineShaderStageCreateInfo voxStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    voxStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    voxStage.module = ctx.voxShaderModule;
    voxStage.pName = "VoxelizeMain";
    VkComputePipelineCreateInfo voxCpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    voxCpci.stage = voxStage;
    voxCpci.layout = ctx.voxPipelineLayout;
    vkCheck(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &voxCpci, nullptr,
                                      &ctx.voxPipeline),
            "vkCreateComputePipelines(voxelize)");

    VkDescriptorPoolSize voxPoolSizes[2]{};
    voxPoolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    voxPoolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo voxDpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    voxDpci.maxSets = 1;
    voxDpci.poolSizeCount = 2;
    voxDpci.pPoolSizes = voxPoolSizes;
    vkCheck(vkCreateDescriptorPool(ctx.device, &voxDpci, nullptr, &ctx.voxDescPool),
            "vkCreateDescriptorPool(voxelize)");

    VkDescriptorSetAllocateInfo voxDsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    voxDsai.descriptorPool = ctx.voxDescPool;
    voxDsai.descriptorSetCount = 1;
    voxDsai.pSetLayouts = &ctx.voxDescSetLayout;
    vkCheck(vkAllocateDescriptorSets(ctx.device, &voxDsai, &ctx.voxDescSet),
            "vkAllocateDescriptorSets(voxelize)");

    // --- Mesh + scan pipelines: MeshCountMain + MeshEmitMain + ScanBlocks/
    // Sums/AddMain, shared superset layout (see GpuContext comment), three
    // descriptor sets with identical buffer wiring so each kernel only
    // differs by pipeline bind. Bindings 9/10 are the scan extension.
    VkDescriptorSetLayoutBinding meshBindings[7]{};
    meshBindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    meshBindings[1] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    meshBindings[2] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    meshBindings[3] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    meshBindings[4] = {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    meshBindings[5] = {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    meshBindings[6] = {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo meshDslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    meshDslci.bindingCount = 7;
    meshDslci.pBindings = meshBindings;
    vkCheck(vkCreateDescriptorSetLayout(ctx.device, &meshDslci, nullptr, &ctx.meshDescSetLayout),
            "vkCreateDescriptorSetLayout(mesh)");

    VkPipelineLayoutCreateInfo meshPlci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    meshPlci.setLayoutCount = 1;
    meshPlci.pSetLayouts = &ctx.meshDescSetLayout;
    vkCheck(vkCreatePipelineLayout(ctx.device, &meshPlci, nullptr, &ctx.meshPipelineLayout),
            "vkCreatePipelineLayout(mesh)");

    const auto makeMeshPipeline = [&](const std::string& path, const char* entry,
                                       VkShaderModule& outModule, VkPipeline& outPipeline) {
        const std::vector<uint8_t> code = readFile(path);
        if (code.size() % 4 != 0) fail("SPIR-V binary size not a multiple of 4: " + path);
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = code.size();
        mci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        vkCheck(vkCreateShaderModule(ctx.device, &mci, nullptr, &outModule),
                "vkCreateShaderModule(mesh)");
        VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        st.module = outModule;
        st.pName = entry;
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage = st;
        pci.layout = ctx.meshPipelineLayout;
        vkCheck(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, nullptr,
                                          &outPipeline),
                "vkCreateComputePipelines(mesh)");
    };
    makeMeshPipeline(meshCountSpvPath, "MeshCountMain", ctx.meshCountModule,
                     ctx.meshCountPipeline);
    makeMeshPipeline(meshEmitSpvPath, "MeshEmitMain", ctx.meshEmitModule, ctx.meshEmitPipeline);
    makeMeshPipeline(scanBlocksSpvPath, "ScanBlocksMain", ctx.scanBlocksModule,
                     ctx.scanBlocksPipeline);
    makeMeshPipeline(scanSumsSpvPath, "ScanSumsMain", ctx.scanSumsModule, ctx.scanSumsPipeline);
    makeMeshPipeline(scanAddSpvPath, "ScanAddMain", ctx.scanAddModule, ctx.scanAddPipeline);

    // 3 sets (count/emit/scan) x 7 bindings/set (1 uniform + 6 storage).
    VkDescriptorPoolSize meshPoolSizes[2]{};
    meshPoolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3};
    meshPoolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 18};
    VkDescriptorPoolCreateInfo meshDpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    meshDpci.maxSets = 3;
    meshDpci.poolSizeCount = 2;
    meshDpci.pPoolSizes = meshPoolSizes;
    vkCheck(vkCreateDescriptorPool(ctx.device, &meshDpci, nullptr, &ctx.meshDescPool),
            "vkCreateDescriptorPool(mesh)");

    VkDescriptorSetLayout meshLayouts[3] = {ctx.meshDescSetLayout, ctx.meshDescSetLayout,
                                             ctx.meshDescSetLayout};
    VkDescriptorSet meshSets[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorSetAllocateInfo meshDsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    meshDsai.descriptorPool = ctx.meshDescPool;
    meshDsai.descriptorSetCount = 3;
    meshDsai.pSetLayouts = meshLayouts;
    vkCheck(vkAllocateDescriptorSets(ctx.device, &meshDsai, meshSets),
            "vkAllocateDescriptorSets(mesh)");
    ctx.meshCountSet = meshSets[0];
    ctx.meshEmitSet = meshSets[1];
    ctx.scanSet = meshSets[2];

    return ctx;
}

void destroyContext(GpuContext& ctx) {
    if (ctx.device) {
        if (ctx.timestampPool) vkDestroyQueryPool(ctx.device, ctx.timestampPool, nullptr);
        if (ctx.meshDescPool) vkDestroyDescriptorPool(ctx.device, ctx.meshDescPool, nullptr);
        if (ctx.scanBlocksPipeline) vkDestroyPipeline(ctx.device, ctx.scanBlocksPipeline, nullptr);
        if (ctx.scanSumsPipeline) vkDestroyPipeline(ctx.device, ctx.scanSumsPipeline, nullptr);
        if (ctx.scanAddPipeline) vkDestroyPipeline(ctx.device, ctx.scanAddPipeline, nullptr);
        if (ctx.scanBlocksModule) vkDestroyShaderModule(ctx.device, ctx.scanBlocksModule, nullptr);
        if (ctx.scanSumsModule) vkDestroyShaderModule(ctx.device, ctx.scanSumsModule, nullptr);
        if (ctx.scanAddModule) vkDestroyShaderModule(ctx.device, ctx.scanAddModule, nullptr);
        if (ctx.meshCountPipeline) vkDestroyPipeline(ctx.device, ctx.meshCountPipeline, nullptr);
        if (ctx.meshEmitPipeline) vkDestroyPipeline(ctx.device, ctx.meshEmitPipeline, nullptr);
        if (ctx.meshCountModule) vkDestroyShaderModule(ctx.device, ctx.meshCountModule, nullptr);
        if (ctx.meshEmitModule) vkDestroyShaderModule(ctx.device, ctx.meshEmitModule, nullptr);
        if (ctx.meshPipelineLayout)
            vkDestroyPipelineLayout(ctx.device, ctx.meshPipelineLayout, nullptr);
        if (ctx.meshDescSetLayout)
            vkDestroyDescriptorSetLayout(ctx.device, ctx.meshDescSetLayout, nullptr);
        // vkDestroyDescriptorPool implicitly frees ctx.descSet / ctx.voxDescSet.
        if (ctx.voxDescPool) vkDestroyDescriptorPool(ctx.device, ctx.voxDescPool, nullptr);
        if (ctx.voxPipeline) vkDestroyPipeline(ctx.device, ctx.voxPipeline, nullptr);
        if (ctx.voxPipelineLayout)
            vkDestroyPipelineLayout(ctx.device, ctx.voxPipelineLayout, nullptr);
        if (ctx.voxShaderModule) vkDestroyShaderModule(ctx.device, ctx.voxShaderModule, nullptr);
        if (ctx.voxDescSetLayout)
            vkDestroyDescriptorSetLayout(ctx.device, ctx.voxDescSetLayout, nullptr);
        if (ctx.descPool) vkDestroyDescriptorPool(ctx.device, ctx.descPool, nullptr);
        if (ctx.pipeline) vkDestroyPipeline(ctx.device, ctx.pipeline, nullptr);
        if (ctx.pipelineLayout) vkDestroyPipelineLayout(ctx.device, ctx.pipelineLayout, nullptr);
        if (ctx.shaderModule) vkDestroyShaderModule(ctx.device, ctx.shaderModule, nullptr);
        if (ctx.descSetLayout)
            vkDestroyDescriptorSetLayout(ctx.device, ctx.descSetLayout, nullptr);
        if (ctx.commandPool) vkDestroyCommandPool(ctx.device, ctx.commandPool, nullptr);
        vkDestroyDevice(ctx.device, nullptr);
    }
    if (ctx.instance) vkDestroyInstance(ctx.instance, nullptr);
    ctx = GpuContext{};
}

// --- one dispatch region ----------------------------------------------------

// SyntheticTileSampler with its elevations divided down by a constant.
//
// WHY A HARNESS-LOCAL SAMPLER EXISTS AT ALL (worldgen v10). v10's curvature gate
// is a RAMP between two clamps, and on SyntheticTileSampler's raster it is
// pinned at a clamp in 100% of the columns this harness compares -- measured,
// not guessed: tier-normalised |curvature| runs 3,295..46,094 q10 against a knee
// of 512, i.e. 6x to 90x saturated, on every region and at both pitches. The
// sampler is a COVERAGE sampler and its terrain is far rougher than the
// realistic 30 m raster kCurvatureKneeQ10 was measured against (mean 288 q10).
//
// A saturated gate still catches a sign error -- crests clamp high, hollows
// clamp low, and the regions below do both -- but it cannot catch an error in
// the MAGNITUDE of anything upstream of it: a wrong basis denominator, a dropped
// stage-1 divide or a mis-scaled tier normalisation all still saturate, and all
// still pass. It also makes the two rounding choices in that chain
// (carrierCurvatureTierNormQ10's floorDiv, and evalSurface's truncating
// `(cScale - 1024) / 2`) unobservable, because the excursion is only ever the
// clamp's own even value. Both were confirmed invisible by swapping the helper
// in worldgen.ush and watching the gate still pass.
//
// Dividing the raster down moves curvature into the ramp without touching
// worldgen, tiles.h, or the existing fixtures: it is just a gentler world, and
// both the CPU reference and the uploaded GPU raster read it through this same
// object, so it is as deterministic as the sampler it wraps. A divisor of 1 is
// the identity (floorDiv by 1), so the pre-existing regions are bit-unchanged.
class ScaledTileSampler final : public ITileSampler {
public:
    ScaledTileSampler(uint64_t seed, int32_t pixelSizeMm, int64_t elevationDivisor)
        : inner_(seed, pixelSizeMm), divisor_(elevationDivisor) {}

    int32_t pixelSizeMm() const override { return inner_.pixelSizeMm(); }
    int32_t elevationMm(int64_t px, int64_t py) override {
        // floorDiv, not `/`: elevations are routinely negative (this sampler
        // makes oceans), and a truncating divide would round the two sides of
        // sea level differently -- the exact operand pattern docs/determinism.md
        // bans. The divisor is a fixture constant, so this is deterministic.
        return static_cast<int32_t>(floorDiv(inner_.elevationMm(px, py), divisor_));
    }
    ClimateSample climate(int64_t px, int64_t py) override { return inner_.climate(px, py); }

private:
    SyntheticTileSampler inner_;
    int64_t divisor_;
};

struct RegionSpec {
    const char* name;
    int32_t originVx, originVy;
    uint32_t width, height; // dispatch columns
    // Tile raster pitch for this fixture's SyntheticTileSampler, and therefore
    // WorldGenParams::PixelSizeMm.
    //
    // WHY THIS IS PER-REGION RATHER THAN ONE CONSTANT (worldgen v10). evalSurface
    // now BRANCHES on the pitch: vxc::isFineTier(pxMm) (pxMm <= 3750) selects
    // kFineDetailOctaves -- a different table, a different length, a different
    // band split, and a different channel-to-lattice mapping -- from
    // kDetailOctaves. Both branches are mirrored in worldgen.ush, and a mirror
    // of a branch that only one side ever takes is not a mirror that has been
    // tested. Before this field every fixture ran at 30000 and the entire fine
    // ladder was unexercised on the GPU: it would have compiled, linted and
    // PASSED with the fine table transposed, mis-channelled or simply absent.
    int32_t pixelSizeMm;
    // Elevation divisor for ScaledTileSampler; 1 is the identity. See that
    // class's comment for why a gentler raster is the only way this harness can
    // reach the INTERIOR of v10's curvature ramp rather than only its clamps.
    int64_t elevationDivisor;
};

struct RegionResult {
    std::vector<GpuColumnSample> samples; // width*height, row-major (x fast)
    std::vector<ColumnSample> cpuCols;    // width*height CPU reference columns, same order/index
    std::vector<uint32_t> cells;          // OutCells readback; layout doc near cellIndexInBrick()
    int32_t brickZMin = 0;
    uint32_t bricksZ = 0;
    double columnDispatchMs = 0;   // wall-clock: ColumnMain submit -> fence signalled
    double voxelizeDispatchMs = 0; // wall-clock: VoxelizeMain submit -> fence signalled

    // Mesh passes (docs/gpu-mesher-design.md): quads for INTERIOR bricks only
    // (1-brick halo excluded on every axis), packed 2x uint32 per quad in
    // deterministic count->GPU-scan->emit order (see runMeshChain()).
    std::vector<uint64_t> quads; // packed (word0 | word1<<32), totalQuads entries
    uint32_t totalQuads = 0;
    uint32_t interiorBricksX = 0, interiorBricksY = 0, interiorBricksZ = 0;
    double meshCountDispatchMs = 0;
    double meshScanBlocksDispatchMs = 0;
    double meshScanSumsDispatchMs = 0;
    double meshScanAddDispatchMs = 0;
    double meshEmitDispatchMs = 0;
};

// --- shared mesh+scan chain (used by both runRegion() and gate::runTile()) -

// Buffer handles the chained mesh/scan dispatches read or write. cells is
// read-only (VoxelizeMain's output); counts/offsets/blockSums/quads are
// read-write across the chain -- see the binding table in the GpuContext
// comment above.
struct MeshBuffers {
    VkBuffer cells;
    VkBuffer counts;
    VkBuffer offsets;
    VkBuffer blockSums;
    VkBuffer quads;
};

struct MeshStageTimingsMs {
    double count = 0, scanBlocks = 0, scanSums = 0, scanAdd = 0, emit = 0;
};

// Writes the full 7-binding superset (see GpuContext comment) into all THREE
// mesh/scan descriptor sets (meshCountSet, meshEmitSet, scanSet) -- their
// content is IDENTICAL; they stay separate VkDescriptorSet objects only so
// vkCmdBindDescriptorSets can pick the one for the currently-bound pipeline,
// mirroring the pre-existing meshCountSet/meshEmitSet split. Binding 6
// (offsets, read-only view for MeshEmitMain's InQuadOffsets) and binding 9
// (offsets, read-write view for the scan kernels' OutQuadOffsets) both point
// at mb.offsets -- the SAME VkBuffer bound twice in one set, once per role.
void writeMeshDescriptors(GpuContext& ctx, VkBuffer paramsBuffer, const MeshBuffers& mb) {
    VkDescriptorBufferInfo pInfo{paramsBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo cInfo{mb.cells, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo oReadInfo{mb.offsets, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo nInfo{mb.counts, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo qInfo{mb.quads, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo oWriteInfo{mb.offsets, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bInfo{mb.blockSums, 0, VK_WHOLE_SIZE};

    const VkDescriptorSet sets[3] = {ctx.meshCountSet, ctx.meshEmitSet, ctx.scanSet};
    for (VkDescriptorSet set : sets) {
        VkWriteDescriptorSet w[7]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &pInfo, nullptr};
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 5, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &cInfo, nullptr};
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 6, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &oReadInfo, nullptr};
        w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 7, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &nInfo, nullptr};
        w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 8, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &qInfo, nullptr};
        w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 9, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &oWriteInfo, nullptr};
        w[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 10, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bInfo, nullptr};
        vkUpdateDescriptorSets(ctx.device, 7, w, 0, nullptr);
    }
}

// Records, submits and waits on ONE command buffer chaining all 5 mesh/scan
// dispatches (MeshCountMain -> ScanBlocksMain -> ScanSumsMain -> ScanAddMain
// -> MeshEmitMain) with COMPUTE->COMPUTE buffer barriers between every stage
// (write->read on exactly the buffer(s) the next stage reads) and a single
// trailing COMPUTE->HOST barrier, so there is exactly ONE fence for the whole
// chain (replaces the old host prefix scan between separately-submitted
// count and emit passes -- see docs/status.md's M0 gate row). Quad order is
// unchanged: MeshEmitMain still writes at exactly the offsets an exclusive
// scan of MeshCountMain's counts would produce, just computed on GPU instead
// of the CPU walking mapped memory between two submissions.
//
// ScanSumsMain exclusive-scans OutBlockSums in a SINGLE workgroup (256
// threads => up to 256 blocks of 256 masks = 65,536 masks); the gate mode's
// z-slab splitting (see ZWindow in the gate section) and the 64x64 default
// regions both keep maskCount under that, but callers must never dispatch
// more — offsets for masks past the capacity would silently miss their
// scanned block base and corrupt MeshEmitMain's write positions.
//
// Per-stage GPU timing without extra submits: 6 timestamp queries bracket
// the 5 dispatches inside the single command buffer (see GpuContext's
// timestampPool comment); vkGetQueryPoolResults after the fence turns the 6
// raw ticks into 5 stage deltas.
MeshStageTimingsMs runMeshChain(GpuContext& ctx, uint32_t maskCount, const MeshBuffers& mb) {
    if (maskCount > 65536) {
        fail("mesh maskCount " + std::to_string(maskCount) +
             " exceeds ScanSumsMain's single-workgroup limit of 65536 (256 blocks x 256 "
             "threads/block) -- dispatch footprint too large for the GPU scan");
    }
    const uint32_t meshGroups = (maskCount + 63) / 64;   // Mesh{Count,Emit}Main: numthreads(64,1,1)
    const uint32_t scanGroups = (maskCount + 255) / 256; // Scan{Blocks,Add}Main: numthreads(256,1,1)

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = ctx.commandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(ctx.device, &cbai, &cmd),
            "vkAllocateCommandBuffers(mesh-chain)");
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(cmd, &cbbi), "vkBeginCommandBuffer(mesh-chain)");

    vkCmdResetQueryPool(cmd, ctx.timestampPool, 0, 6);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ctx.timestampPool, 0);

    const auto bufBarrier = [](VkBuffer buf, VkAccessFlags src,
                                VkAccessFlags dst) -> VkBufferMemoryBarrier {
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = buf;
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
        return b;
    };

    // Visibility for this chain's first read of the voxelized cells (written
    // by VoxelizeMain in an earlier, already-fenced submission).
    {
        VkBufferMemoryBarrier b =
            bufBarrier(mb.cells, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &b, 0,
                              nullptr);
    }

    // --- MeshCountMain: cells -> OutQuadCounts ------------------------------
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshCountPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout, 0, 1,
                             &ctx.meshCountSet, 0, nullptr);
    vkCmdDispatch(cmd, meshGroups, 1, 1);
    {
        VkBufferMemoryBarrier b =
            bufBarrier(mb.counts, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &b, 0,
                              nullptr);
    }
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx.timestampPool, 1);

    // --- ScanBlocksMain: OutQuadCounts -> OutQuadOffsets (per-block) + OutBlockSums
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.scanBlocksPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout, 0, 1,
                             &ctx.scanSet, 0, nullptr);
    vkCmdDispatch(cmd, scanGroups, 1, 1);
    {
        VkBufferMemoryBarrier bs[2] = {
            bufBarrier(mb.offsets, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            bufBarrier(mb.blockSums, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2, bs, 0,
                              nullptr);
    }
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx.timestampPool, 2);

    // --- ScanSumsMain: OutBlockSums exclusive-scanned in place, 1 workgroup -
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.scanSumsPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout, 0, 1,
                             &ctx.scanSet, 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);
    {
        VkBufferMemoryBarrier b =
            bufBarrier(mb.blockSums, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &b, 0,
                              nullptr);
    }
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx.timestampPool, 3);

    // --- ScanAddMain: OutQuadOffsets += scanned block base ------------------
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.scanAddPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout, 0, 1,
                             &ctx.scanSet, 0, nullptr);
    vkCmdDispatch(cmd, scanGroups, 1, 1);
    {
        VkBufferMemoryBarrier b =
            bufBarrier(mb.offsets, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &b, 0,
                              nullptr);
    }
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx.timestampPool, 4);

    // --- MeshEmitMain: writes quads at the GPU-scanned offsets --------------
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshEmitPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout, 0, 1,
                             &ctx.meshEmitSet, 0, nullptr);
    vkCmdDispatch(cmd, meshGroups, 1, 1);
    {
        // Host needs: quads (the emitted stream) and counts+offsets (to
        // derive the true total = counts[maskCount-1] + offsets[maskCount-1],
        // since the quads buffer is upper-bound sized, not exactly sized --
        // see the runRegion()/runTile() callers).
        VkBufferMemoryBarrier bs[3] = {
            bufBarrier(mb.quads, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT),
            bufBarrier(mb.counts, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT),
            bufBarrier(mb.offsets, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT)};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, nullptr, 3, bs, 0, nullptr);
    }
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx.timestampPool, 5);

    vkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(mesh-chain)");

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCheck(vkCreateFence(ctx.device, &fci, nullptr, &fence), "vkCreateFence(mesh-chain)");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkCheck(vkQueueSubmit(ctx.queue, 1, &si, fence), "vkQueueSubmit(mesh-chain)");
    vkCheck(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX),
            "vkWaitForFences(mesh-chain)");
    vkDestroyFence(ctx.device, fence, nullptr);

    uint64_t ts[6] = {};
    vkCheck(vkGetQueryPoolResults(ctx.device, ctx.timestampPool, 0, 6, sizeof(ts), ts,
                                   sizeof(uint64_t),
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
            "vkGetQueryPoolResults(mesh-chain)");
    const double toMs = ctx.timestampPeriodNs / 1.0e6;
    MeshStageTimingsMs t;
    t.count = double(ts[1] - ts[0]) * toMs;
    t.scanBlocks = double(ts[2] - ts[1]) * toMs;
    t.scanSums = double(ts[3] - ts[2]) * toMs;
    t.scanAdd = double(ts[4] - ts[3]) * toMs;
    t.emit = double(ts[5] - ts[4]) * toMs;
    return t;
}

RegionResult runRegion(GpuContext& ctx, const RegionSpec& region, uint64_t seed) {
    if (region.width % 8 != 0 || region.height % 8 != 0) {
        fail(std::string("region '") + region.name +
             "' dispatch footprint must be brick-aligned (width/height multiples of 8) "
             "per worldgen.ush's VoxelizeMain contract");
    }

    ScaledTileSampler tiles(seed, region.pixelSizeMm, region.elevationDivisor);
    const int64_t pixelSizeMm = tiles.pixelSizeMm();

    // Raster window covering every bilinear tap the dispatch touches
    // (worldgen.ush's documented contract): pixel range from the column mm
    // range, +1 on the high end for the second bilinear tap, and
    // kRasterCavernMarginMm on both ends for VoxelizeMain's site-surface taps.
    const int64_t xMmMin = int64_t(region.originVx) * kVoxelSizeMm - kRasterCavernMarginMm;
    const int64_t xMmMax =
        int64_t(region.originVx + int32_t(region.width) - 1) * kVoxelSizeMm + kRasterCavernMarginMm;
    const int64_t yMmMin = int64_t(region.originVy) * kVoxelSizeMm - kRasterCavernMarginMm;
    const int64_t yMmMax =
        int64_t(region.originVy + int32_t(region.height) - 1) * kVoxelSizeMm + kRasterCavernMarginMm;
    const int64_t pxMin = floorDiv(xMmMin, pixelSizeMm) + kCarrierStencilLo;
    const int64_t pxMax = floorDiv(xMmMax, pixelSizeMm) + kCarrierStencilHi;
    const int64_t pyMin = floorDiv(yMmMin, pixelSizeMm) + kCarrierStencilLo;
    const int64_t pyMax = floorDiv(yMmMax, pixelSizeMm) + kCarrierStencilHi;
    const uint32_t rasterW = static_cast<uint32_t>(pxMax - pxMin + 1);
    const uint32_t rasterH = static_cast<uint32_t>(pyMax - pyMin + 1);

    std::printf("[%s] origin (%d,%d) %ux%u columns, raster window (%lld,%lld) %ux%u px, "
                "pitch %lld mm (%s tier)\n",
                region.name, region.originVx, region.originVy, region.width, region.height,
                (long long)pxMin, (long long)pyMin, rasterW, rasterH, (long long)pixelSizeMm,
                pixelSizeMm <= 3750 ? "FINE" : "coarse");

    // CPU columns for the WHOLE region, computed once up front. This serves
    // two purposes and both need it before the GPU dispatch: (a) it derives
    // VoxelizeMain's z-range the same way GeneratedWorld<8>::surfaceBrickRange
    // does (top-voxel min/max -> brick z range), except over the whole region
    // rather than a single 8x8 footprint, since BrickZMin/BricksZ are one
    // pair of scalars shared by every footprint in the dispatch; (b) it is
    // returned to the caller and reused as ground truth for BOTH the
    // ColumnMain field comparison and the VoxelizeMain cell comparison, so
    // vxc::Amplifier::column is never computed twice for the same column.
    ScaledTileSampler cpuTiles(seed, region.pixelSizeMm, region.elevationDivisor);
    Amplifier cpuAmp(seed, cpuTiles);
    std::vector<ColumnSample> cpuCols(size_t(region.width) * region.height);
    int64_t vzMin = INT64_MAX, vzMax = INT64_MIN;
    for (uint32_t y = 0; y < region.height; ++y) {
        for (uint32_t x = 0; x < region.width; ++x) {
            const int64_t vx = int64_t(region.originVx) + x;
            const int64_t vy = int64_t(region.originVy) + y;
            const ColumnSample c = cpuAmp.column(vx, vy);
            cpuCols[size_t(x) + size_t(y) * region.width] = c;
            // Topmost solid voxel: centre (vz*100+50) <= surfaceMm, WIDENED by
            // the v12 3D density band's 7-voxel envelope either side (mirrors
            // GeneratedWorld<8>::surfaceBrickRange, which was widened the same
            // way and for the same two reasons -- the top voxel moves, and the
            // bricks under it stop being homogeneous).
            //
            // Until v20 it did a third job: widening by the 3D density band's
            // envelope so the band's own cells were INSIDE the compared buffer.
            // The term is gone (core.h v20) and so is the widening. The lesson
            // survives it -- a per-voxel term is only actually compared if the
            // range being compared covers the voxels it touches.
            const int64_t top = floorDiv(int64_t(c.surfaceMm) - kVoxelSizeMm / 2, kVoxelSizeMm);
            vzMin = top < vzMin ? top : vzMin;
            vzMax = top > vzMax ? top : vzMax;
        }
    }
    const int32_t brickZMin = static_cast<int32_t>(floorDiv(vzMin, 8));
    const int32_t brickZMax = static_cast<int32_t>(floorDiv(vzMax, 8));
    const uint32_t bricksZ = static_cast<uint32_t>(brickZMax - brickZMin + 1);
    const uint32_t bricksX = region.width / 8;
    const uint32_t bricksY = region.height / 8;
    std::printf("[%s] voxelize z-range: brick z [%d, %d] (%u bricks tall)\n", region.name,
                brickZMin, brickZMax, bricksZ);

    Buffer paramsBuf = ctx.createBuffer(sizeof(WorldGenParamsCB),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    Buffer elevBuf = ctx.createBuffer(VkDeviceSize(rasterW) * rasterH * sizeof(int32_t),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer climBuf = ctx.createBuffer(VkDeviceSize(rasterW) * rasterH * sizeof(uint32_t),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    // Columns buffer is shared: ColumnMain writes it (OutColumns, u0), then
    // the SAME VkBuffer is bound as VoxelizeMain's InColumns (t3) — chained
    // dispatch, columns never recomputed on GPU.
    Buffer columnsBuf = ctx.createBuffer(
        VkDeviceSize(region.width) * region.height * sizeof(GpuColumnSample),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer cellsBuf = ctx.createBuffer(
        VkDeviceSize(bricksX) * bricksY * bricksZ * 512 * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // Fill the raster window exactly as Amplifier::column would read it
    // through the same SyntheticTileSampler (elevation mm, packed climate
    // t | s<<8 | p<<16 | v<<24 — matches worldgen.ush's cl unpack).
    int32_t* elev = static_cast<int32_t*>(elevBuf.mapped);
    uint32_t* clim = static_cast<uint32_t*>(climBuf.mapped);
    for (uint32_t ly = 0; ly < rasterH; ++ly) {
        for (uint32_t lx = 0; lx < rasterW; ++lx) {
            const int64_t px = pxMin + lx, py = pyMin + ly;
            const size_t idx = size_t(lx) + size_t(ly) * rasterW;
            elev[idx] = tiles.elevationMm(px, py);
            const ClimateSample cl = tiles.climate(px, py);
            clim[idx] = uint32_t(cl.temperature) | (uint32_t(cl.seasonality) << 8) |
                        (uint32_t(cl.precipitation) << 16) |
                        (uint32_t(cl.precipVariability) << 24);
        }
    }

    WorldGenParamsCB params{};
    // D5: coarse cell size in level-0 voxels (1 << level). MUST be 1 for the
    // level-0 fixtures -- coarseRep multiplies by it, so the zero a
    // value-initialised struct leaves here collapses EVERY column to world
    // origin (0,0). That is exactly what it did: the gate compared a CPU
    // reference walking real coordinates against a GPU that evaluated (0,0)
    // for all 4096 columns, and it stayed hidden because rasterElevationMm
    // clamps to each region's own window, so the wrong answer still varied
    // per region and looked plausible.
    params.CoarseScale = 1;
    params.RingSkirtMask = 0;
    params.DispatchColumnsX = region.width;
    params.DispatchColumnsY = region.height;
    params.RasterOriginPxX = static_cast<int32_t>(pxMin);
    params.RasterOriginPxY = static_cast<int32_t>(pyMin);
    params.RasterSizeX = rasterW;
    params.RasterSizeY = rasterH;
    params.PixelSizeMm = static_cast<int32_t>(pixelSizeMm);
    params.SeedLo = static_cast<uint32_t>(seed & 0xffffffffu);
    params.SeedHi = static_cast<uint32_t>(seed >> 32);
    params.OriginVx = region.originVx;
    params.OriginVy = region.originVy;
    params.BrickZMin = brickZMin;
    params.BricksZ = bricksZ;
    validateWorldGenParams(params, "runRegion");
    std::memcpy(paramsBuf.mapped, &params, sizeof(params));

    // ColumnMain descriptor set: unchanged bindings/order, columnsBuf in the
    // OutColumns (u0) role.
    VkDescriptorBufferInfo paramsInfo{paramsBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo elevInfo{elevBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo climInfo{climBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo columnsInfo{columnsBuf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[4]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.descSet, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &paramsInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.descSet, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &elevInfo, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.descSet, 2, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &climInfo, nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.descSet, 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &columnsInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 4, writes, 0, nullptr);

    // VoxelizeMain descriptor set: binding 0 shares the SAME paramsBuf,
    // binding 1 (ElevationMm) the SAME elevBuf ColumnMain reads (the cavern
    // pass's site-surface evaluation), binding 4 (InColumns) the SAME
    // columnsBuf ColumnMain just wrote, binding 5 (OutCells) the new cellsBuf.
    VkDescriptorBufferInfo cellsInfo{cellsBuf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet voxWrites[4]{};
    voxWrites[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.voxDescSet, 0, 0, 1,
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &paramsInfo, nullptr};
    voxWrites[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.voxDescSet, 1, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &elevInfo, nullptr};
    voxWrites[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.voxDescSet, 4, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &columnsInfo, nullptr};
    voxWrites[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.voxDescSet, 5, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &cellsInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 4, voxWrites, 0, nullptr);

    const uint32_t groupsX = (region.width + 7) / 8;
    const uint32_t groupsY = (region.height + 7) / 8;

    // --- Pass 1: ColumnMain -------------------------------------------------
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = ctx.commandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd1 = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(ctx.device, &cbai, &cmd1), "vkAllocateCommandBuffers(column)");

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(cmd1, &cbbi), "vkBeginCommandBuffer(column)");
    vkCmdBindPipeline(cmd1, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipeline);
    vkCmdBindDescriptorSets(cmd1, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelineLayout, 0, 1,
                             &ctx.descSet, 0, nullptr);
    vkCmdDispatch(cmd1, groupsX, groupsY, 1);

    // Visibility barrier: SHADER_WRITE -> HOST_READ | SHADER_READ. HOST_READ
    // so the host can map columnsBuf post-fence (HOST_COHERENT memory skips
    // flush/invalidate calls, not the Vulkan memory-domain visibility op);
    // SHADER_READ so this write is also available to VoxelizeMain's read of
    // the same buffer as InColumns in the next submission (see the repeated
    // barrier at the top of cmd2, which performs that read's visibility op).
    VkBufferMemoryBarrier barrier1{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier1.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier1.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier1.buffer = columnsBuf.buffer;
    barrier1.offset = 0;
    barrier1.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd1, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                          nullptr, 1, &barrier1, 0, nullptr);
    vkCheck(vkEndCommandBuffer(cmd1), "vkEndCommandBuffer(column)");

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence1 = VK_NULL_HANDLE;
    vkCheck(vkCreateFence(ctx.device, &fci, nullptr, &fence1), "vkCreateFence(column)");

    VkSubmitInfo submit1{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit1.commandBufferCount = 1;
    submit1.pCommandBuffers = &cmd1;

    const auto tColumn = Clock::now();
    vkCheck(vkQueueSubmit(ctx.queue, 1, &submit1, fence1), "vkQueueSubmit(column)");
    vkCheck(vkWaitForFences(ctx.device, 1, &fence1, VK_TRUE, UINT64_MAX), "vkWaitForFences(column)");
    const double columnDispatchMs = msSince(tColumn);
    vkDestroyFence(ctx.device, fence1, nullptr);

    // --- Pass 2: VoxelizeMain, chained off cmd1's columnsBuf write ---------
    VkCommandBuffer cmd2 = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(ctx.device, &cbai, &cmd2), "vkAllocateCommandBuffers(voxelize)");
    vkCheck(vkBeginCommandBuffer(cmd2, &cbbi), "vkBeginCommandBuffer(voxelize)");

    // Availability was already established by barrier1 above (its dst scope
    // included SHADER_READ); this repeats the dependency as the visibility
    // operation for THIS submission's reads, per the Vulkan memory model —
    // correct regardless of the (already GPU-idle) execution ordering from
    // the fence wait.
    VkBufferMemoryBarrier chainBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    chainBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    chainBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    chainBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    chainBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    chainBarrier.buffer = columnsBuf.buffer;
    chainBarrier.offset = 0;
    chainBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd2, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &chainBarrier, 0,
                          nullptr);

    vkCmdBindPipeline(cmd2, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.voxPipeline);
    vkCmdBindDescriptorSets(cmd2, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.voxPipelineLayout, 0, 1,
                             &ctx.voxDescSet, 0, nullptr);
    vkCmdDispatch(cmd2, groupsX, groupsY, 1);

    VkBufferMemoryBarrier barrier2{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier2.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier2.buffer = cellsBuf.buffer;
    barrier2.offset = 0;
    barrier2.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd2, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                          0, nullptr, 1, &barrier2, 0, nullptr);
    vkCheck(vkEndCommandBuffer(cmd2), "vkEndCommandBuffer(voxelize)");

    VkFence fence2 = VK_NULL_HANDLE;
    vkCheck(vkCreateFence(ctx.device, &fci, nullptr, &fence2), "vkCreateFence(voxelize)");

    VkSubmitInfo submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit2.commandBufferCount = 1;
    submit2.pCommandBuffers = &cmd2;

    const auto tVoxelize = Clock::now();
    vkCheck(vkQueueSubmit(ctx.queue, 1, &submit2, fence2), "vkQueueSubmit(voxelize)");
    vkCheck(vkWaitForFences(ctx.device, 1, &fence2, VK_TRUE, UINT64_MAX),
            "vkWaitForFences(voxelize)");
    const double voxelizeDispatchMs = msSince(tVoxelize);
    vkDestroyFence(ctx.device, fence2, nullptr);
    // Command buffers are reclaimed when ctx.commandPool is destroyed; no
    // explicit vkFreeCommandBuffers needed for this one-shot-per-region use.

    RegionResult result;
    result.columnDispatchMs = columnDispatchMs;
    result.voxelizeDispatchMs = voxelizeDispatchMs;
    result.samples.resize(size_t(region.width) * region.height);
    std::memcpy(result.samples.data(), columnsBuf.mapped,
                result.samples.size() * sizeof(GpuColumnSample));
    result.cpuCols = std::move(cpuCols);
    result.cells.resize(size_t(bricksX) * bricksY * bricksZ * 512);
    std::memcpy(result.cells.data(), cellsBuf.mapped, result.cells.size() * sizeof(uint32_t));
    result.brickZMin = brickZMin;
    result.bricksZ = bricksZ;

    // --- Passes 3-7: MeshCountMain -> GPU scan (ScanBlocks/Sums/AddMain) ->
    // MeshEmitMain, chained in ONE command buffer via runMeshChain() --------
    // (docs/gpu-mesher-design.md: deterministic ordering with no atomics;
    // see runMeshChain()'s doc comment for the barrier/timing design).
    if (bricksX >= 3 && bricksY >= 3 && bricksZ >= 3) {
        const uint32_t mbx = bricksX - 2, mby = bricksY - 2, mbz = bricksZ - 2;
        const uint32_t maskCount = mbx * mby * mbz * 48u;
        result.interiorBricksX = mbx;
        result.interiorBricksY = mby;
        result.interiorBricksZ = mbz;

        Buffer countsBuf = ctx.createBuffer(VkDeviceSize(maskCount) * sizeof(uint32_t),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Buffer offsetsBuf = ctx.createBuffer(VkDeviceSize(maskCount) * sizeof(uint32_t),
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Buffer blockSumsBuf = ctx.createBuffer(VkDeviceSize((maskCount + 255) / 256) * sizeof(uint32_t),
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        // Quads buffer MUST be sized before the chain is recorded (MeshEmitMain
        // is in the same command buffer as MeshCountMain, so there is no
        // readback point between "how many quads" and "write them"). Upper
        // bound: each mask emits at most 32 quads (docs/gpu-mesher-design.md).
        // Grown-only reuse doesn't apply here (runRegion() creates fresh
        // buffers per call) but the bound itself is small: 128x128 columns
        // top out around 9-10k masks, i.e. a few MB.
        Buffer quadsBuf = ctx.createBuffer(VkDeviceSize(maskCount) * 32 * sizeof(uint64_t),
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        // ScanBlocks/Sums/AddMain read ScanCount from the SAME WorldGenParams
        // buffer ColumnMain/VoxelizeMain already consumed; safe to update the
        // host-mapped copy now (host writes before vkQueueSubmit are made
        // available per the Vulkan memory model, same as the initial upload).
        params.ScanCount = maskCount;
        validateScanCount(params.ScanCount, maskCount, "runRegion mesh chain");
        std::memcpy(paramsBuf.mapped, &params, sizeof(params));

        writeMeshDescriptors(ctx, paramsBuf.buffer,
                              {cellsBuf.buffer, countsBuf.buffer, offsetsBuf.buffer,
                               blockSumsBuf.buffer, quadsBuf.buffer});
        const MeshStageTimingsMs stageMs = runMeshChain(
            ctx, maskCount,
            {cellsBuf.buffer, countsBuf.buffer, offsetsBuf.buffer, blockSumsBuf.buffer,
             quadsBuf.buffer});
        result.meshCountDispatchMs = stageMs.count;
        result.meshScanBlocksDispatchMs = stageMs.scanBlocks;
        result.meshScanSumsDispatchMs = stageMs.scanSums;
        result.meshScanAddDispatchMs = stageMs.scanAdd;
        result.meshEmitDispatchMs = stageMs.emit;

        // True total = counts[maskCount-1] + offsets[maskCount-1] (exclusive
        // scan tail); the quads buffer itself is upper-bound sized.
        const uint32_t* counts = static_cast<const uint32_t*>(countsBuf.mapped);
        const uint32_t* offsets = static_cast<const uint32_t*>(offsetsBuf.mapped);
        const uint32_t running = counts[maskCount - 1] + offsets[maskCount - 1];
        result.totalQuads = running;
        if (running > 0) {
            result.quads.resize(running);
            std::memcpy(result.quads.data(), quadsBuf.mapped, size_t(running) * sizeof(uint64_t));
        }

        ctx.destroyBuffer(countsBuf);
        ctx.destroyBuffer(offsetsBuf);
        ctx.destroyBuffer(blockSumsBuf);
        ctx.destroyBuffer(quadsBuf);
    } else {
        std::printf("[%s] region too thin for interior mesh bricks (bricksZ=%u) — mesh pass "
                    "skipped\n",
                    region.name, bricksZ);
    }

    ctx.destroyBuffer(paramsBuf);
    ctx.destroyBuffer(elevBuf);
    ctx.destroyBuffer(climBuf);
    ctx.destroyBuffer(columnsBuf);
    ctx.destroyBuffer(cellsBuf);

    return result;
}

// --- comparison against the CPU reference -----------------------------------

struct Mismatch {
    int64_t vx, vy;
    std::string field;
    int64_t cpuVal, gpuVal;
};

// ============================================================================
// --radius gate mode (M0 gate): the FULL GPU pipeline (ColumnMain ->
// VoxelizeMain -> MeshCount/EmitMain) over every surface-shell brick within a
// horizontal radius of the origin. Tiles the target square into fixed
// 128x128-column dispatches (runRegion() above uses one such dispatch per
// call); a shared 1-brick halo on every tile means each tile's 14x14-brick
// interior (the same "interior brick" concept the mesh pass above already
// uses) is owned by exactly one tile, so interiors partition the target area
// with no gaps and no double-meshing.
// ============================================================================
namespace gate {

constexpr int32_t kBrick = 8;
constexpr int32_t kTileBricks = 16;                                // 128 columns / 8
constexpr int32_t kHaloBricks = 1;                                 // shared 1-brick halo
constexpr int32_t kInteriorBricks = kTileBricks - 2 * kHaloBricks; // 14 owned bricks/tile
constexpr int32_t kTileColumns = kTileBricks * kBrick;             // 128
constexpr int32_t kVerifySampleStride = 8;   // "every 8th tile" sampled verification
constexpr int64_t kFullVerifyRadiusM = 64;   // <= this radius: full CPU comparison

// A buffer allocated once (lazily, on first need) that only ever grows:
// later tiles reuse the same VkBuffer/VkDeviceMemory as long as it still
// fits, instead of the per-region create/destroy runRegion() uses for its
// single dispatch. Vulkan buffer+memory (de)allocation is heavy enough that
// repeating it ~500 times (once per tile at --radius 128) would swamp the
// gate number with allocator overhead unrelated to the pipeline being
// measured. Only cellsBuf/countsBuf/offsetsBuf/blockSumsBuf/quadsBuf
// actually vary in size tile-to-tile (they scale with each tile's local
// vertical extent, bricksZ, which depends on nearby terrain height) —
// elevBuf/climBuf also use this so a rare +/-1 raster-window rounding
// difference between tiles doesn't need special-casing either.
// paramsBuf/columnsBuf are genuinely fixed-size (128x128 columns, every
// tile) and are allocated once, plainly, outside this helper.
struct GrowBuffer {
    Buffer buf;
    VkDeviceSize capacityBytes = 0;
    VkBufferUsageFlags usage = 0;
    size_t reallocCount = 0;

    // Returns true iff the underlying VkBuffer was (re)allocated -- callers
    // use this to skip vkUpdateDescriptorSets when a slot's buffer set is
    // unchanged from the previous tile that used this slot (persistent
    // descriptor sets across the batched-flight run; see FlightSlot below).
    // Growth policy. Allocating EXACTLY `bytes` meant a slot that saw tiles of
    // steadily increasing bricksZ/maskCount re-allocated on almost every one:
    // measured at --radius 64, 202 reallocations across the 16 slots, each
    // costing ~1.7 ms of vkAllocateMemory + vkMapMemory (+ unmap/free of the
    // old buffer) on tens of MB of host-visible memory. Only 112 of those
    // (7 buffers x 16 slots) are unavoidable first touches.
    //
    // So over-allocate: at least 1.5x the previous capacity, rounded up to a
    // 1 MB granularity. That bounds the wasted memory at 50% while making
    // repeated small growths free. Buffer CAPACITY is invisible to the
    // shaders -- every dispatch is bounded by WorldGenParams (DispatchColumns,
    // BricksZ, ScanCount), never by buffer size -- so this cannot move a
    // digest. It only ever allocates MORE than before.
    static VkDeviceSize grownCapacity(VkDeviceSize bytes, VkDeviceSize prevCapacity) {
        const VkDeviceSize slack = prevCapacity + prevCapacity / 2;
        VkDeviceSize want = bytes > slack ? bytes : slack;
        constexpr VkDeviceSize kGranularity = 1u << 20; // 1 MB
        return (want + kGranularity - 1) / kGranularity * kGranularity;
    }

    bool ensure(GpuContext& ctx, VkDeviceSize bytes, double* allocMsOut = nullptr) {
        if (bytes <= capacityBytes && buf.buffer != VK_NULL_HANDLE) return false;
        const auto t0 = Clock::now();
        if (buf.buffer != VK_NULL_HANDLE) ctx.destroyBuffer(buf);
        capacityBytes = grownCapacity(bytes, capacityBytes);
        buf = ctx.createBuffer(capacityBytes, usage);
        ++reallocCount;
        if (allocMsOut) *allocMsOut += msSince(t0);
        return true;
    }
};

struct GateBuffers {
    Buffer paramsBuf;                   // fixed: sizeof(WorldGenParamsCB)
    GrowBuffer elevBuf, climBuf;        // raster window (rarely grows past the first tile)
    Buffer columnsBuf;                  // fixed: kTileColumns^2 * sizeof(GpuColumnSample)
    GrowBuffer cellsBuf;                // grows to the tallest bricksZ seen
    GrowBuffer countsBuf, offsetsBuf;   // mesh mask buffers, sized off bricksZ too
    GrowBuffer blockSumsBuf;            // GPU-scan per-256-block totals, (maskCount+255)/256 uints
    GrowBuffer quadsBuf;                // emitted quads, upper-bound sized off each tile's maskCount

    size_t totalReallocs() const {
        return elevBuf.reallocCount + climBuf.reallocCount + cellsBuf.reallocCount +
               countsBuf.reallocCount + offsetsBuf.reallocCount + blockSumsBuf.reallocCount +
               quadsBuf.reallocCount;
    }
};

struct TileSpec {
    int32_t tx = 0, ty = 0;
    int32_t originVx = 0, originVy = 0; // dispatch tile origin, halo included
    int32_t ownedBx0 = 0, ownedBy0 = 0; // first interior (owned) brick, global brick coords
};

// ---------------------------------------------------------------------------
// Batched-flight tiling (closes the 128m gate's remaining marshalling +
// per-tile-fence overhead): tiles are processed in flights of kFlightSize,
// each flight recording every tile's FULL chain (ColumnMain -> VoxelizeMain
// -> MeshCount -> GPU scan -> MeshEmit) into ONE command buffer with ONE
// fence, instead of the old 3-fences-per-tile (column, voxelize, mesh chain)
// scheme. A flight needs kFlightSize independent buffer/descriptor sets
// (FlightSlot) since all its tiles are simultaneously resident on the GPU
// within that one submission.
//
// CPU/GPU overlap ("flight k+1 prep while flight k's fence is pending, via a
// deferred wait") additionally needs the slot SET flight k+1 writes into to
// be different from the one flight k's GPU commands may still be reading —
// reusing the very same kFlightSize slots for both would race the CPU's
// host-visible-memory writes against the GPU still consuming them. So slots
// are double-buffered: kPipelineDepth (2) banks of kFlightSize slots each,
// flights alternate banks, and a bank is only reused once its owning
// flight's fence has been waited on (which always happens, in the main loop
// below, before that bank is prepared again two flights later). Digest/
// compare order is untouched: flights are built from grid.tiles in order and
// harvested (in that same order) strictly before the flight two iterations
// later reuses their bank, so the CPU-side digest byte stream is identical
// to the old fully-serial per-tile loop.
constexpr int32_t kFlightSize = 8;
constexpr int32_t kPipelineDepth = 2;
constexpr int32_t kTimestampsPerTile = 8; // brackets 7 stages: col,vox,count,scanB,scanS,scanA,emit

struct FlightSlot {
    GateBuffers gb;
    VkDescriptorSet colSet = VK_NULL_HANDLE;
    VkDescriptorSet voxSet = VK_NULL_HANDLE;
    VkDescriptorSet meshCountSet = VK_NULL_HANDLE;
    VkDescriptorSet meshEmitSet = VK_NULL_HANDLE;
    VkDescriptorSet scanSet = VK_NULL_HANDLE;
    bool colVoxDescriptorsWritten = false;  // persistent sets: only rewrite on first use / regrow
    bool meshDescriptorsWritten = false;

    // Per-tile scratch: filled by prepTileCpu() during flight prep, consumed
    // by recordTileCommands() (same flight) and harvestTile() (after the
    // flight's fence signals). Not touched while this slot's bank is
    // "in flight" on the GPU from a PRIOR flight -- see kPipelineDepth above.
    TileSpec tile;
    bool active = false; // false for the unused tail slots of a partial last flight
    bool verify = false;
    std::vector<ColumnSample> cpuCols;
    int32_t brickZMin = 0;
    uint32_t bricksX = 0, bricksY = 0, bricksZ = 0;
    uint32_t mbx = 0, mby = 0, mbz = 0, maskCount = 0;
    uint32_t running = 0; // true quad total for this tile, known only after the fence
};

// Dedicated descriptor pool for the flight banks, sized for
// kPipelineDepth*kFlightSize tiles' worth of sets. Allocated separately from
// GpuContext's own pools (used by runRegion()'s default mode and left
// untouched) so this optimization is fully additive/isolated to gate mode.
struct FlightDescriptors {
    VkDescriptorPool pool = VK_NULL_HANDLE;
};

FlightDescriptors createFlightDescriptors(GpuContext& ctx, int32_t totalSlots,
                                           std::vector<FlightSlot>& slots) {
    FlightDescriptors fd;
    // Per tile: colSet (1 uniform + 3 storage), voxSet (1 uniform + 3 storage
    // -- elevation joined it with the C6 cavern mirror),
    // meshCountSet/meshEmitSet/scanSet (1 uniform + 6 storage each, x3) --
    // mirrors createContext()'s pool-size derivation, just x totalSlots.
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uint32_t(totalSlots) * 5};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32_t(totalSlots) * 24};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = uint32_t(totalSlots) * 5;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = poolSizes;
    vkCheck(vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &fd.pool),
            "vkCreateDescriptorPool(flight)");

    const auto allocDescriptorSets = [&](VkDescriptorSetLayout layout) -> std::vector<VkDescriptorSet> {
        std::vector<VkDescriptorSetLayout> setLayouts;
        setLayouts.assign(size_t(totalSlots), layout);
        std::vector<VkDescriptorSet> result;
        result.resize(size_t(totalSlots));
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = fd.pool;
        dsai.descriptorSetCount = uint32_t(totalSlots);
        dsai.pSetLayouts = setLayouts.data();
        vkCheck(vkAllocateDescriptorSets(ctx.device, &dsai, result.data()),
                "vkAllocateDescriptorSets(flight)");
        return result;
    };
    const std::vector<VkDescriptorSet> colSets = allocDescriptorSets(ctx.descSetLayout);
    const std::vector<VkDescriptorSet> voxSets = allocDescriptorSets(ctx.voxDescSetLayout);
    const std::vector<VkDescriptorSet> meshCountSets = allocDescriptorSets(ctx.meshDescSetLayout);
    const std::vector<VkDescriptorSet> meshEmitSets = allocDescriptorSets(ctx.meshDescSetLayout);
    const std::vector<VkDescriptorSet> scanSets = allocDescriptorSets(ctx.meshDescSetLayout);
    for (int32_t i = 0; i < totalSlots; ++i) {
        FlightSlot& s = slots[size_t(i)];
        s.colSet = colSets[size_t(i)];
        s.voxSet = voxSets[size_t(i)];
        s.meshCountSet = meshCountSets[size_t(i)];
        s.meshEmitSet = meshEmitSets[size_t(i)];
        s.scanSet = scanSets[size_t(i)];
    }
    return fd;
}

void destroyFlightDescriptors(GpuContext& ctx, FlightDescriptors& fd) {
    if (fd.pool) vkDestroyDescriptorPool(ctx.device, fd.pool, nullptr); // frees all sets too
    fd = FlightDescriptors{};
}

// Persistent-descriptor variants of writeMeshDescriptors()/the inline column+
// voxelize descriptor writes in runRegion(): write ONE slot's 2 (col+vox) or
// 3 (mesh) descriptor sets, called only when that slot's buffers actually
// changed (first use, or a GrowBuffer regrow) -- not every tile, per the
// "marshalling reduction" goal.
void writeColVoxDescriptorsSlot(GpuContext& ctx, FlightSlot& s) {
    VkDescriptorBufferInfo paramsInfo{s.gb.paramsBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo elevInfo{s.gb.elevBuf.buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo climInfo{s.gb.climBuf.buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo columnsInfo{s.gb.columnsBuf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[4]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.colSet, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &paramsInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.colSet, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &elevInfo, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.colSet, 2, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &climInfo, nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.colSet, 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &columnsInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 4, writes, 0, nullptr);

    VkDescriptorBufferInfo cellsInfo{s.gb.cellsBuf.buf.buffer, 0, VK_WHOLE_SIZE};
    // Binding 1 (ElevationMm) is bound to the voxelize set too since the C6
    // cavern mirror — see the voxBindings comment in initVulkan().
    VkWriteDescriptorSet voxWrites[4]{};
    voxWrites[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.voxSet, 0, 0, 1,
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &paramsInfo, nullptr};
    voxWrites[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.voxSet, 1, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &elevInfo, nullptr};
    voxWrites[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.voxSet, 4, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &columnsInfo, nullptr};
    voxWrites[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.voxSet, 5, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &cellsInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 4, voxWrites, 0, nullptr);
}

void writeMeshDescriptorsSlot(GpuContext& ctx, FlightSlot& s) {
    VkDescriptorBufferInfo pInfo{s.gb.paramsBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo cInfo{s.gb.cellsBuf.buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo oReadInfo{s.gb.offsetsBuf.buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo nInfo{s.gb.countsBuf.buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo qInfo{s.gb.quadsBuf.buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo oWriteInfo{s.gb.offsetsBuf.buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bInfo{s.gb.blockSumsBuf.buf.buffer, 0, VK_WHOLE_SIZE};

    const VkDescriptorSet sets[3] = {s.meshCountSet, s.meshEmitSet, s.scanSet};
    for (VkDescriptorSet set : sets) {
        VkWriteDescriptorSet w[7]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &pInfo, nullptr};
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 5, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &cInfo, nullptr};
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 6, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &oReadInfo, nullptr};
        w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 7, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &nInfo, nullptr};
        w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 8, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &qInfo, nullptr};
        w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 9, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &oWriteInfo, nullptr};
        w[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 10, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bInfo, nullptr};
        vkUpdateDescriptorSets(ctx.device, 7, w, 0, nullptr);
    }
}

struct TileGrid {
    int32_t bMin = 0, bMax = 0;  // global brick-coord bounds of the requested radius (square)
    int32_t numTiles = 0;        // per axis; grid is numTiles x numTiles
    std::vector<TileSpec> tiles; // row-major, ty outer, tx inner
};

// bMin/bMax mirror bench_main.cpp's brick-range derivation exactly (same
// floorDiv(-radiusVox,B)/floorDiv(radiusVox-1,B) formula) so the CPU-radius
// baseline and this GPU gate describe the same nominal footprint. Tiling
// that range into fixed-size 128-column/16-brick dispatches with a 14-brick
// interior means the LAST tile in each row/column can overshoot bMax by up
// to kInteriorBricks-1 bricks when totalBricks isn't an exact multiple of
// 14 (true for both 64m and 128m here) — deliberate: interiors must tile
// contiguously with no gap, and slightly overshooting the far edge is
// harmless (it's still real, deterministically-generated terrain, just
// outside the nominal radius) whereas a gap or a double-meshed brick would
// not be.
TileGrid buildTileGrid(int64_t radiusM) {
    TileGrid g;
    const int64_t radiusVox = radiusM * 1000 / kVoxelSizeMm;
    g.bMin = static_cast<int32_t>(floorDiv(-radiusVox, kBrick));
    g.bMax = static_cast<int32_t>(floorDiv(radiusVox - 1, kBrick));
    const int32_t totalBricks = g.bMax - g.bMin + 1;
    g.numTiles = (totalBricks + kInteriorBricks - 1) / kInteriorBricks;
    g.tiles.reserve(size_t(g.numTiles) * size_t(g.numTiles));
    for (int32_t ty = 0; ty < g.numTiles; ++ty) {
        for (int32_t tx = 0; tx < g.numTiles; ++tx) {
            TileSpec t;
            t.tx = tx;
            t.ty = ty;
            t.ownedBx0 = g.bMin + tx * kInteriorBricks;
            t.ownedBy0 = g.bMin + ty * kInteriorBricks;
            t.originVx = (t.ownedBx0 - kHaloBricks) * kBrick;
            t.originVy = (t.ownedBy0 - kHaloBricks) * kBrick;
            g.tiles.push_back(t);
        }
    }
    return g;
}

struct GateStats {
    // Setup + verification costs, timed but EXCLUDED from the gate number.
    double cpuColumnPassMs = 0; // per-tile z-range CPU column pass (existing pattern)
    double cpuCompareMs = 0;    // CPU-vs-GPU comparison + digest bookkeeping

    // Per-stage GPU busy-time totals, summed across every tile, from
    // vkCmdWriteTimestamp queries bracketing EVERY stage now (column,
    // voxelize, meshcount, 3x scan, emit) -- not just the mesh chain as
    // before. These are true GPU execution times regardless of how much CPU
    // marshalling of a later flight overlapped with them, so (unlike the old
    // fully-serial per-tile loop) their sum can legitimately exceed the
    // headline gate number below once flight pipelining hides marshalling
    // time inside GPU execution time (see runGateMode's report).
    double hostRasterFillMs = 0, marshallingMs = 0, bufferAllocMs = 0, columnMs = 0,
           voxelizeMs = 0, meshCountMs = 0, scanBlocksMs = 0, scanSumsMs = 0, scanAddMs = 0,
           meshEmitMs = 0;
    // The headline gate number: total loop wall-clock MINUS the two costs
    // explicitly excluded per the gate spec (cpuColumnPassMs, cpuCompareMs).
    // Computed once in runGateMode from the measured wall clock, not summed
    // per-tile/per-flight -- see runGateMode for why (flight pipelining means
    // per-tile windows overlap, so summing them would double-count).
    double gateMs = 0;

    size_t totalTiles = 0, verifiedTiles = 0;
    size_t totalColumns = 0, verifiedColumns = 0;
    size_t totalCells = 0, verifiedCells = 0;
    size_t totalQuads = 0, verifiedQuads = 0;
    size_t totalInteriorBricks = 0;

    Digest digest;
    std::vector<Mismatch> mismatches;
};

// --- Flight-batched tile pipeline (replaces the old one-tile-at-a-time
// runTile(), which paid 3 fences/tile -- column, voxelize, mesh chain -- and
// rewrote every descriptor set every tile). Split into three phases so
// kFlightSize tiles can share ONE command buffer / ONE fence, and so flight
// k+1's CPU-only prep can overlap flight k's still-pending fence (see
// FlightSlot's doc comment above for the double-buffering/race-safety
// reasoning):
//   prepTileCpu()        - CPU column pass, raster fill, buffer ensure,
//                          WorldGenParams upload, descriptor writes IFF this
//                          slot's buffers actually (re)allocated. No GPU
//                          submission.
//   recordTileCommands() - records this tile's 7-dispatch chain (column,
//                          voxelize, meshcount, 3x scan, emit) with barriers
//                          and 8 bracketing timestamps into the flight's
//                          shared command buffer.
//   harvestTile()         - after the flight's fence has signalled: computes
//                          the true quad total from the fenced counts/
//                          offsets buffers, then runs the exact same digest/
//                          CPU-compare logic runTile() used to run after
//                          "GATE WINDOW ENDS" -- byte-for-byte unchanged, so
//                          the digest is identical to the old serial loop as
//                          long as tiles are harvested in tile order (they
//                          are; see runGateMode()).

// A tile's dispatch z-window. Normally the tile's full surface-straddling
// brick range, but tiles whose terrain relief makes that range too tall for
// the GPU scan's single-workgroup capacity (ScanSumsMain: 256 blocks x 256
// masks = 65,536 masks; 14x14-brick interiors give 9,408 masks per interior
// z-layer, so > 6 interior layers overflows) are split into z-SLABS of at
// most kMaxSlabInteriorLayers interior layers each. Slab interiors partition
// the tile's interior layers exactly (no gap, no overlap) and adjacent slabs
// share a 1-brick halo, mirroring how tile interiors partition the plane —
// so every brick is still meshed exactly once and apron reads stay inside
// the slab's own voxelized range. Before worldgen v3's spectral-gap terrain
// no gate tile ever exceeded 6 interior layers, so the overflow was
// unreachable; v3's rougher relief made it routine (125/144 tiles at
// --radius 64), and without this split the unscanned block bases silently
// corrupted MeshEmitMain's write offsets (the default-regions path has
// always FATALed loudly on the same condition — see runMeshChain()).
struct ZWindow {
    int32_t brickZMin = 0;
    uint32_t bricksZ = 0;
};
constexpr uint32_t kMaxSlabInteriorLayers = 6; // 14*14*6*48 = 56,448 <= 65,536

void prepTileCpu(GpuContext& ctx, FlightSlot& s, const TileSpec& tile, uint64_t seed,
                  SyntheticTileSampler& tiles, Amplifier& amp, bool verify, GateStats& stats,
                  const ZWindow* forcedWindow, std::vector<ZWindow>& overflowSlabs) {
    constexpr int32_t W = kTileColumns, H = kTileColumns;
    const int64_t pixelSizeMm = tiles.pixelSizeMm();

    s.tile = tile;
    s.active = true;
    s.verify = verify;

    // Both ends carry kRasterCavernMarginMm for VoxelizeMain's site-surface
    // taps — see that constant's comment.
    const int64_t xMmMin = int64_t(tile.originVx) * kVoxelSizeMm - kRasterCavernMarginMm;
    const int64_t xMmMax = int64_t(tile.originVx + W - 1) * kVoxelSizeMm + kRasterCavernMarginMm;
    const int64_t yMmMin = int64_t(tile.originVy) * kVoxelSizeMm - kRasterCavernMarginMm;
    const int64_t yMmMax = int64_t(tile.originVy + H - 1) * kVoxelSizeMm + kRasterCavernMarginMm;
    const int64_t pxMin = floorDiv(xMmMin, pixelSizeMm) + kCarrierStencilLo;
    const int64_t pxMax = floorDiv(xMmMax, pixelSizeMm) + kCarrierStencilHi;
    const int64_t pyMin = floorDiv(yMmMin, pixelSizeMm) + kCarrierStencilLo;
    const int64_t pyMax = floorDiv(yMmMax, pixelSizeMm) + kCarrierStencilHi;
    const uint32_t rasterW = static_cast<uint32_t>(pxMax - pxMin + 1);
    const uint32_t rasterH = static_cast<uint32_t>(pyMax - pyMin + 1);

    // --- CPU column pass (setup, excluded from the gate number) -----------
    const auto tCpu0 = Clock::now();
    s.cpuCols.resize(size_t(W) * H);
    int64_t vzMin = INT64_MAX, vzMax = INT64_MIN;
    for (int32_t y = 0; y < H; ++y) {
        for (int32_t x = 0; x < W; ++x) {
            const int64_t vx = int64_t(tile.originVx) + x;
            const int64_t vy = int64_t(tile.originVy) + y;
            const ColumnSample c = amp.column(vx, vy);
            s.cpuCols[size_t(x) + size_t(y) * W] = c;
            // Mirrors runRegion's copy above; the v12 3D-density widening both
            // carried was removed at v20 with the term.
            const int64_t top = floorDiv(int64_t(c.surfaceMm) - kVoxelSizeMm / 2, kVoxelSizeMm);
            vzMin = top < vzMin ? top : vzMin;
            vzMax = top > vzMax ? top : vzMax;
        }
    }
    stats.cpuColumnPassMs += msSince(tCpu0);

    if (forcedWindow) {
        // A z-slab of a tall tile: the window was derived (below) when the
        // full tile was first prepped; use it verbatim.
        s.brickZMin = forcedWindow->brickZMin;
        s.bricksZ = forcedWindow->bricksZ;
    } else {
        s.brickZMin = static_cast<int32_t>(floorDiv(vzMin, kBrick));
        const int32_t brickZMax = static_cast<int32_t>(floorDiv(vzMax, kBrick));
        s.bricksZ = static_cast<uint32_t>(brickZMax - s.brickZMin + 1);
        // Too tall for one GPU-scan dispatch? Keep the FIRST slab for this
        // slot and hand the rest back for the caller to enqueue (processed
        // next, ascending z, so the digest/compare order stays fully
        // deterministic).
        const uint32_t mbzNat = s.bricksZ >= 3 ? s.bricksZ - 2 : 0;
        if (mbzNat > kMaxSlabInteriorLayers) {
            const uint32_t numSlabs =
                (mbzNat + kMaxSlabInteriorLayers - 1) / kMaxSlabInteriorLayers;
            const int32_t z0 = s.brickZMin;
            for (uint32_t k = 1; k < numSlabs; ++k) {
                const uint32_t ic =
                    std::min(kMaxSlabInteriorLayers, mbzNat - k * kMaxSlabInteriorLayers);
                ZWindow w;
                w.brickZMin = z0 + int32_t(k * kMaxSlabInteriorLayers);
                w.bricksZ = ic + 2;
                overflowSlabs.push_back(w);
            }
            s.bricksZ = kMaxSlabInteriorLayers + 2; // slab 0: interiors [1, 1+6)
        }
    }
    s.bricksX = uint32_t(W) / kBrick;
    s.bricksY = uint32_t(H) / kBrick; // == 16

    // --- Buffer ensure/grow (persistent slot buffers; only actually
    // (re)allocates on this slot's first use or when a tile needs more room
    // than any previous tile that used this slot). Timed into its OWN bucket:
    // this is vkAllocateMemory/vkMapMemory cost, not marshalling, and lumping
    // the two together made the old single "marshalling" line read as though
    // per-tile CPU marshalling cost 342 ms when almost all of it was 202
    // allocations. -------------------------------------------------------
    double allocMs = 0.0;
    const bool cellsGrew = s.gb.cellsBuf.ensure(
        ctx, VkDeviceSize(s.bricksX) * s.bricksY * s.bricksZ * 512 * sizeof(uint32_t), &allocMs);
    const bool elevGrew =
        s.gb.elevBuf.ensure(ctx, VkDeviceSize(rasterW) * rasterH * sizeof(int32_t), &allocMs);
    const bool climGrew =
        s.gb.climBuf.ensure(ctx, VkDeviceSize(rasterW) * rasterH * sizeof(uint32_t), &allocMs);

    const auto tRaster0 = Clock::now();
    int32_t* elev = static_cast<int32_t*>(s.gb.elevBuf.buf.mapped);
    uint32_t* clim = static_cast<uint32_t*>(s.gb.climBuf.buf.mapped);
    for (uint32_t ly = 0; ly < rasterH; ++ly) {
        for (uint32_t lx = 0; lx < rasterW; ++lx) {
            const int64_t px = pxMin + lx, py = pyMin + ly;
            const size_t idx = size_t(lx) + size_t(ly) * rasterW;
            elev[idx] = tiles.elevationMm(px, py);
            const ClimateSample cl = tiles.climate(px, py);
            clim[idx] = uint32_t(cl.temperature) | (uint32_t(cl.seasonality) << 8) |
                        (uint32_t(cl.precipitation) << 16) | (uint32_t(cl.precipVariability) << 24);
        }
    }
    stats.hostRasterFillMs += msSince(tRaster0);

    // --- Marshalling proper: WorldGenParams fill/upload and persistent
    // descriptor writes (only when a bound buffer actually changed). The mesh
    // mask buffers' ensure() calls below add into allocMs, not this. -------
    const double allocMsBeforeMarshal = allocMs; // part-A allocs are outside the bracket below
    const auto tMarshal0 = Clock::now();
    WorldGenParamsCB params{};
    params.DispatchColumnsX = uint32_t(W);
    params.DispatchColumnsY = uint32_t(H);
    params.RasterOriginPxX = static_cast<int32_t>(pxMin);
    params.RasterOriginPxY = static_cast<int32_t>(pyMin);
    params.RasterSizeX = rasterW;
    params.RasterSizeY = rasterH;
    params.PixelSizeMm = static_cast<int32_t>(pixelSizeMm);
    params.SeedLo = static_cast<uint32_t>(seed & 0xffffffffu);
    params.SeedHi = static_cast<uint32_t>(seed >> 32);
    params.OriginVx = tile.originVx;
    params.OriginVy = tile.originVy;
    params.BrickZMin = s.brickZMin;
    params.BricksZ = s.bricksZ;
    // See the note at the other params site: CoarseScale is a MULTIPLIER, so
    // leaving the value-initialised 0 collapses every column to world origin.
    params.CoarseScale = 1;
    params.RingSkirtMask = 0;
    validateWorldGenParams(params, "prepTileCpu");

    if (!s.colVoxDescriptorsWritten || elevGrew || climGrew || cellsGrew) {
        writeColVoxDescriptorsSlot(ctx, s);
        s.colVoxDescriptorsWritten = true;
        s.meshDescriptorsWritten = false; // cellsBuf is shared with the mesh set too
    }

    s.mbx = s.mby = s.mbz = s.maskCount = 0;
    if (s.bricksX >= 3 && s.bricksY >= 3 && s.bricksZ >= 3) {
        s.mbx = s.bricksX - 2;
        s.mby = s.bricksY - 2;
        s.mbz = s.bricksZ - 2;
        s.maskCount = s.mbx * s.mby * s.mbz * 48u;
        // The z-slab split above guarantees this; a failure here means the
        // slab math regressed. Same 65,536 contract runMeshChain() enforces
        // for the default-regions path — never dispatch past the GPU scan's
        // single-workgroup capacity, it silently corrupts emit offsets.
        if (s.maskCount > 65536)
            fail("prepTileCpu: maskCount " + std::to_string(s.maskCount) +
                 " exceeds ScanSumsMain's 65,536-mask capacity after slab split "
                 "(tile(" + std::to_string(tile.tx) + "," + std::to_string(tile.ty) +
                 ") bricksZ=" + std::to_string(s.bricksZ) + ")");

        const bool countsGrew =
            s.gb.countsBuf.ensure(ctx, VkDeviceSize(s.maskCount) * sizeof(uint32_t), &allocMs);
        const bool offsetsGrew =
            s.gb.offsetsBuf.ensure(ctx, VkDeviceSize(s.maskCount) * sizeof(uint32_t), &allocMs);
        const bool blockSumsGrew = s.gb.blockSumsBuf.ensure(
            ctx, VkDeviceSize((s.maskCount + 255) / 256) * sizeof(uint32_t), &allocMs);
        // Upper bound (32 quads/mask max, docs/gpu-mesher-design.md) since
        // MeshEmitMain is chained into the SAME command buffer as
        // MeshCountMain -- no readback point to size it exactly first.
        const bool quadsGrew = s.gb.quadsBuf.ensure(
            ctx, VkDeviceSize(s.maskCount) * 32 * sizeof(uint64_t), &allocMs);

        params.ScanCount = s.maskCount;
        validateScanCount(params.ScanCount, s.maskCount, "prepTileCpu mesh chain");

        if (!s.meshDescriptorsWritten || cellsGrew || countsGrew || offsetsGrew || blockSumsGrew ||
            quadsGrew) {
            writeMeshDescriptorsSlot(ctx, s);
            s.meshDescriptorsWritten = true;
        }
    }

    std::memcpy(s.gb.paramsBuf.mapped, &params, sizeof(params));
    // msSince(tMarshal0) spans the mesh ensure() calls too, so subtract the
    // allocation time they contributed; the remainder is params + descriptors.
    const double marshalOnlyMs = msSince(tMarshal0) - (allocMs - allocMsBeforeMarshal);
    stats.marshallingMs += marshalOnlyMs > 0.0 ? marshalOnlyMs : 0.0;
    stats.bufferAllocMs += allocMs;
}

// Records this tile's full chain into the flight's shared command buffer,
// using slot s's persistent descriptor sets (already written by
// prepTileCpu()). tsBase is this tile's offset into the flight's timestamp
// query pool (kTimestampsPerTile slots, reset once per flight before any
// tile is recorded -- see runGateMode()). No inter-tile barriers are needed:
// every tile in a flight uses entirely disjoint buffers, so there is no data
// hazard between them regardless of execution order on the compute queue.
void recordTileCommands(GpuContext& ctx, VkCommandBuffer cmd, FlightSlot& s, VkQueryPool tsPool,
                         uint32_t tsBase) {
    const uint32_t groupsX = (uint32_t(kTileColumns) + 7) / 8, groupsY = groupsX;

    const auto bufBarrier = [](VkBuffer buf, VkAccessFlags src,
                                VkAccessFlags dst) -> VkBufferMemoryBarrier {
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = buf;
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
        return b;
    };
    const auto stageBarrier = [&](VkBufferMemoryBarrier b, VkPipelineStageFlags dstStage) {
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dstStage, 0, 0, nullptr, 1,
                              &b, 0, nullptr);
    };

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, tsPool, tsBase + 0);

    // --- ColumnMain ---------------------------------------------------
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelineLayout, 0, 1,
                             &s.colSet, 0, nullptr);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);
    stageBarrier(bufBarrier(s.gb.columnsBuf.buffer, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT),
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, tsBase + 1);

    // --- VoxelizeMain, chained off this SAME command buffer's ColumnMain --
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.voxPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.voxPipelineLayout, 0, 1,
                             &s.voxSet, 0, nullptr);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);
    stageBarrier(bufBarrier(s.gb.cellsBuf.buf.buffer, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT),
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, tsBase + 2);

    if (s.mbz > 0) {
        const uint32_t meshGroups = (s.maskCount + 63) / 64;
        const uint32_t scanGroups = (s.maskCount + 255) / 256;

        // --- MeshCountMain: cells -> OutQuadCounts -------------------
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshCountPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout, 0, 1,
                                 &s.meshCountSet, 0, nullptr);
        vkCmdDispatch(cmd, meshGroups, 1, 1);
        stageBarrier(
            bufBarrier(s.gb.countsBuf.buf.buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, tsBase + 3);

        // --- ScanBlocksMain: counts -> per-block offsets + block sums -
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.scanBlocksPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout, 0, 1,
                                 &s.scanSet, 0, nullptr);
        vkCmdDispatch(cmd, scanGroups, 1, 1);
        {
            VkBufferMemoryBarrier bs[2] = {
                bufBarrier(s.gb.offsetsBuf.buf.buffer, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT),
                bufBarrier(s.gb.blockSumsBuf.buf.buffer, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT)};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2, bs, 0,
                                  nullptr);
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, tsBase + 4);

        // --- ScanSumsMain: block sums exclusive-scanned in place, 1 wg -
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.scanSumsPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout, 0, 1,
                                 &s.scanSet, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        stageBarrier(bufBarrier(s.gb.blockSumsBuf.buf.buffer, VK_ACCESS_SHADER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT),
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, tsBase + 5);

        // --- ScanAddMain: offsets += scanned block base ---------------
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.scanAddPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout, 0, 1,
                                 &s.scanSet, 0, nullptr);
        vkCmdDispatch(cmd, scanGroups, 1, 1);
        stageBarrier(
            bufBarrier(s.gb.offsetsBuf.buf.buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, tsBase + 6);

        // --- MeshEmitMain: writes quads at the GPU-scanned offsets -----
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshEmitPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout, 0, 1,
                                 &s.meshEmitSet, 0, nullptr);
        vkCmdDispatch(cmd, meshGroups, 1, 1);
        {
            VkBufferMemoryBarrier bs[3] = {
                bufBarrier(s.gb.quadsBuf.buf.buffer, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_HOST_READ_BIT),
                bufBarrier(s.gb.countsBuf.buf.buffer, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_HOST_READ_BIT),
                bufBarrier(s.gb.offsetsBuf.buf.buffer, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_HOST_READ_BIT)};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 3, bs, 0, nullptr);
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, tsBase + 7);
    } else {
        // No interior bricks this tile (bricksZ too thin) -- still write the
        // remaining timestamps so every tile's slice of the query pool has
        // all kTimestampsPerTile slots populated (deltas read ~0 for the
        // skipped stages); keeps harvestTile()'s per-stage readback uniform.
        for (uint32_t k = 3; k <= 7; ++k)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, tsBase + k);
    }
}

// After the owning flight's fence has signalled: computes the true quad
// total from the now-fenced counts/offsets buffers, then digests + (if
// s.verify) CPU-compares this tile's GPU output. Byte-for-byte the same
// logic the old runTile() ran after "GATE WINDOW ENDS".
void harvestTile(FlightSlot& s, GateStats& stats) {
    constexpr int32_t W = kTileColumns, H = kTileColumns;
    const uint32_t bricksX = s.bricksX, bricksY = s.bricksY, bricksZ = s.bricksZ;
    const int32_t brickZMin = s.brickZMin;
    const uint32_t mbx = s.mbx, mby = s.mby, mbz = s.mbz;
    const TileSpec& tile = s.tile;
    const bool verify = s.verify;
    const std::vector<ColumnSample>& cpuCols = s.cpuCols;

    uint32_t running = 0;
    if (mbz > 0) {
        const uint32_t* counts = static_cast<const uint32_t*>(s.gb.countsBuf.buf.mapped);
        const uint32_t* offsets = static_cast<const uint32_t*>(s.gb.offsetsBuf.buf.mapped);
        running = counts[s.maskCount - 1] + offsets[s.maskCount - 1];
    }

    const auto tCmp0 = Clock::now();
    const GpuColumnSample* gpuCols = static_cast<const GpuColumnSample*>(s.gb.columnsBuf.mapped);
    const uint32_t* gpuCells = static_cast<const uint32_t*>(s.gb.cellsBuf.buf.mapped);
    const uint64_t* gpuQuads =
        running > 0 ? static_cast<const uint64_t*>(s.gb.quadsBuf.buf.mapped) : nullptr;

    for (int32_t y = 0; y < H; ++y) {
        for (int32_t x = 0; x < W; ++x) {
            const size_t colIdx = size_t(x) + size_t(y) * W;
            const GpuColumnSample& g = gpuCols[colIdx];
            stats.digest.u32(static_cast<uint32_t>(g.surfaceMm));
            stats.digest.u32(static_cast<uint32_t>(g.topsoilMm));
            stats.digest.u32(static_cast<uint32_t>(g.subsoilMm));
            stats.digest.u32(static_cast<uint32_t>(g.bedrockDepthMm));
            stats.digest.u8(static_cast<uint8_t>(g.surfaceMat));

            const int64_t vx = int64_t(tile.originVx) + x, vy = int64_t(tile.originVy) + y;
            if (verify) {
                const ColumnSample& c = cpuCols[colIdx];
                auto record = [&](const char* field, int64_t cpuVal, int64_t gpuVal) {
                    if (cpuVal != gpuVal && stats.mismatches.size() < 20)
                        stats.mismatches.push_back({vx, vy, field, cpuVal, gpuVal});
                };
                record("surfaceMm", c.surfaceMm, g.surfaceMm);
                record("topsoilMm", c.topsoilMm, g.topsoilMm);
                record("subsoilMm", c.subsoilMm, g.subsoilMm);
                record("bedrockDepthMm", c.bedrockDepthMm, g.bedrockDepthMm);
                record("surfaceMat", c.surfaceMat, g.surfaceMat);
            }

            const uint32_t bx = uint32_t(x) / 8u, by = uint32_t(y) / 8u, lx = uint32_t(x) % 8u,
                           ly = uint32_t(y) % 8u;
            const uint32_t footprintIndex = bx + bricksX * by;
            for (uint32_t bz = 0; bz < bricksZ; ++bz) {
                const size_t brickIndex = size_t(footprintIndex) * bricksZ + bz;
                const int64_t brickZ = int64_t(brickZMin) + int64_t(bz);
                for (uint32_t zLocal = 0; zLocal < 8u; ++zLocal) {
                    const size_t cellIdx = brickIndex * 512 + cellIndexInBrick(lx, ly, zLocal);
                    const uint8_t gpuMat = static_cast<uint8_t>(gpuCells[cellIdx] & 0xffu);
                    stats.digest.u8(gpuMat);
                    if (verify) {
                        const int64_t vz = brickZ * 8 + zLocal;
                        const uint8_t cpuMat =
                            static_cast<uint8_t>(Amplifier::materialAt(cpuCols[colIdx], vz));
                        if (cpuMat != gpuMat && stats.mismatches.size() < 20) {
                            stats.mismatches.push_back(
                                {vx, vy, "cell@vz=" + std::to_string(vz), cpuMat, gpuMat});
                        }
                    }
                }
            }
        }
    }

    stats.totalInteriorBricks += size_t(mbx) * mby * mbz;
    if (mbz > 0) {
        if (verify) {
            // Re-run the CPU mesher per interior brick and cross-check
            // against the GPU quad stream, exactly like the default regions
            // path above — but only for tiles selected for verification
            // (running meshBrick<8> on every brick of every tile would make
            // --radius 128's "sample every 8th tile" pointless).
            std::vector<Quad> cpuQuads;
            size_t gpuCursor = 0;
            for (uint32_t iz = 0; iz < mbz; ++iz) {
                for (uint32_t iy = 0; iy < mby; ++iy) {
                    for (uint32_t ix = 0; ix < mbx; ++ix) {
                        const int64_t ox = (int64_t(ix) + 1) * 8;
                        const int64_t oy = (int64_t(iy) + 1) * 8;
                        const int64_t oz = (int64_t(iz) + 1) * 8;
                        const auto sampler = [&](int sx, int sy, int sz) -> MaterialId {
                            const int64_t rvx = ox + sx, rvy = oy + sy;
                            const int64_t vz = int64_t(brickZMin) * 8 + oz + sz;
                            const ColumnSample& c = cpuCols[size_t(rvx) + size_t(rvy) * W];
                            return Amplifier::materialAt(c, vz);
                        };
                        cpuQuads.clear();
                        meshBrick<8>(sampler, cpuQuads);
                        for (const Quad& q : cpuQuads) {
                            const uint64_t gq =
                                gpuCursor < running ? gpuQuads[gpuCursor] : ~0ull;
                            const uint32_t w0 = uint32_t(gq & 0xffffffffu);
                            const uint32_t w1 = uint32_t(gq >> 32);
                            const uint8_t gAxis = w0 & 0xfu, gDir = (w0 >> 4) & 0xfu,
                                          gSlice = (w0 >> 8) & 0xffu, gU0 = (w0 >> 16) & 0xffu,
                                          gV0 = (w0 >> 24) & 0xffu;
                            const uint8_t gW = w1 & 0xffu, gH = (w1 >> 8) & 0xffu,
                                          gAo = (w1 >> 16) & 0xffu, gMat = (w1 >> 24) & 0xffu;
                            stats.digest.u8(gAxis);
                            stats.digest.u8(gDir);
                            stats.digest.u8(gSlice);
                            stats.digest.u8(gU0);
                            stats.digest.u8(gV0);
                            stats.digest.u8(gW);
                            stats.digest.u8(gH);
                            stats.digest.u8(gAo);
                            stats.digest.u8(gMat);
                            const bool same = gAxis == q.axis && gDir == q.positive &&
                                              gSlice == q.slice && gU0 == q.u0 && gV0 == q.v0 &&
                                              gW == q.w && gH == q.h && gAo == q.ao &&
                                              gMat == q.mat;
                            if (!same && stats.mismatches.size() < 20) {
                                stats.mismatches.push_back(
                                    {int64_t(ix), int64_t(iy),
                                     "quad@tile(" + std::to_string(tile.tx) + "," +
                                         std::to_string(tile.ty) + ")brick(" +
                                         std::to_string(ix) + "," + std::to_string(iy) + "," +
                                         std::to_string(iz) + ")#" + std::to_string(gpuCursor),
                                     int64_t(q.mat), int64_t(gMat)});
                            }
                            ++gpuCursor;
                        }
                    }
                }
            }
            if (gpuCursor != running && stats.mismatches.size() < 20) {
                stats.mismatches.push_back({int64_t(tile.tx), int64_t(tile.ty),
                                             "quadCount total (cpu vs gpu)", int64_t(gpuCursor),
                                             int64_t(running)});
            }
        } else {
            // Not verified: digest the raw GPU quad stream directly (same
            // byte decode/order as the verify branch above) without paying
            // for the CPU mesher.
            for (size_t i = 0; i < running; ++i) {
                const uint64_t gq = gpuQuads[i];
                const uint32_t w0 = uint32_t(gq & 0xffffffffu), w1 = uint32_t(gq >> 32);
                stats.digest.u8(static_cast<uint8_t>(w0 & 0xfu));
                stats.digest.u8(static_cast<uint8_t>((w0 >> 4) & 0xfu));
                stats.digest.u8(static_cast<uint8_t>((w0 >> 8) & 0xffu));
                stats.digest.u8(static_cast<uint8_t>((w0 >> 16) & 0xffu));
                stats.digest.u8(static_cast<uint8_t>((w0 >> 24) & 0xffu));
                stats.digest.u8(static_cast<uint8_t>(w1 & 0xffu));
                stats.digest.u8(static_cast<uint8_t>((w1 >> 8) & 0xffu));
                stats.digest.u8(static_cast<uint8_t>((w1 >> 16) & 0xffu));
                stats.digest.u8(static_cast<uint8_t>((w1 >> 24) & 0xffu));
            }
        }
    }
    stats.cpuCompareMs += msSince(tCmp0);

    ++stats.totalTiles;
    stats.totalColumns += size_t(W) * H;
    stats.totalCells += size_t(bricksX) * bricksY * bricksZ * 512;
    stats.totalQuads += running;
    if (verify) {
        ++stats.verifiedTiles;
        stats.verifiedColumns += size_t(W) * H;
        stats.verifiedCells += size_t(bricksX) * bricksY * bricksZ * 512;
        stats.verifiedQuads += running;
    }
}

int runGateMode(GpuContext& ctx, int64_t radiusM, uint64_t seed) {
    const TileGrid grid = buildTileGrid(radiusM);
    const int32_t coveredBricks = grid.numTiles * kInteriorBricks;
    const double coveredMetres = coveredBricks * kBrick * kVoxelSizeMm / 1000.0;
    const double requestedMetres = double(radiusM) * 2.0;
    const int32_t totalSlots = kFlightSize * kPipelineDepth;
    std::printf(
        "\n=== GATE MODE: radius %lldm (seed %llu) ===\n"
        "tile grid: %dx%d tiles (%zu total), 128x128-column dispatch, 14x14-brick "
        "(112-column) interior, 1-brick shared halo\n"
        "flight batching: %d tiles/flight (1 command buffer, 1 fence), %d-deep pipelined "
        "(flight k+1's CPU prep overlaps flight k's pending fence via a deferred wait), "
        "%d total per-tile buffer/descriptor slots\n"
        "brick coord range [%d, %d] (%d bricks/side); covered square %.1fm/side "
        "(requested %.1fm — the last tile row/col overshoots slightly so interiors stay "
        "brick-aligned and gapless)\n",
        (long long)radiusM, (unsigned long long)seed, grid.numTiles, grid.numTiles,
        grid.tiles.size(), kFlightSize, kPipelineDepth, totalSlots, grid.bMin, grid.bMax,
        grid.bMax - grid.bMin + 1, coveredMetres, requestedMetres);

    const bool fullVerify = radiusM <= kFullVerifyRadiusM;
    std::printf("verification: %s\n",
                fullVerify ? "FULL (every tile)"
                           : "SAMPLED (every 8th tile, deterministic linear tile index)");

    std::vector<FlightSlot> slots;
    slots.resize(size_t(totalSlots));
    for (FlightSlot& s : slots) {
        s.gb.paramsBuf =
            ctx.createBuffer(sizeof(WorldGenParamsCB), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        s.gb.columnsBuf = ctx.createBuffer(
            VkDeviceSize(kTileColumns) * kTileColumns * sizeof(GpuColumnSample),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        s.gb.elevBuf.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        s.gb.climBuf.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        s.gb.cellsBuf.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        s.gb.countsBuf.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        s.gb.offsetsBuf.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        s.gb.blockSumsBuf.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        s.gb.quadsBuf.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    FlightDescriptors fd = createFlightDescriptors(ctx, totalSlots, slots);

    // One timestamp-query pool PER PIPELINE-DEPTH BANK (not one shared pool):
    // a shared pool's reset+write commands from flight k+1 could start
    // executing (same queue, strict submission order, zero gap) before the
    // host finishes vkGetQueryPoolResults for flight k if both flights used
    // the same pool -- see FlightSlot's doc comment on why banks exist at
    // all. Query count only needs to cover kFlightSize tiles (a bank's
    // worth), not totalSlots.
    VkQueryPool tsPools[kPipelineDepth] = {};
    for (int32_t b = 0; b < kPipelineDepth; ++b) {
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = uint32_t(kFlightSize) * uint32_t(kTimestampsPerTile);
        vkCheck(vkCreateQueryPool(ctx.device, &qpci, nullptr, &tsPools[b]),
                "vkCreateQueryPool(flight-timestamps)");
    }

    SyntheticTileSampler tiles(seed);
    Amplifier amp(seed, tiles);

    GateStats stats;
    const size_t numTiles = grid.tiles.size();
    const double toMs = ctx.timestampPeriodNs / 1.0e6;

    // Work stream: tiles in grid order, with a tall tile's extra z-slabs
    // interleaved immediately after its first slab (see ZWindow). The queue
    // only ever holds the current tile's remaining slabs, so consuming it
    // before advancing tileCursor preserves exact deterministic order.
    size_t tileCursor = 0;
    std::vector<ZWindow> pendingSlabs; // FIFO via eraseFront index below
    size_t pendingNext = 0;
    TileSpec pendingTile{};
    bool pendingVerify = false;
    size_t tallTilesSplit = 0, extraSlabs = 0;

    struct PendingFlight {
        bool valid = false;
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        int32_t bank = 0;
        int32_t count = 0;
    };

    // Prepares (CPU column pass + raster fill + buffer/descriptor
    // marshalling for every tile in the flight -- NO GPU submission touched
    // yet) then records + submits (WITHOUT waiting) flight f's single
    // command buffer. Uses bank f % kPipelineDepth; see FlightSlot's doc
    // comment for why that bank is guaranteed free (its previous occupant,
    // flight f-kPipelineDepth, was waited+harvested before this flight two
    // iterations ago in the main loop below).
    const auto workRemaining = [&]() -> bool {
        return pendingNext < pendingSlabs.size() || tileCursor < numTiles;
    };

    const auto prepareAndSubmit = [&](int32_t f) -> PendingFlight {
        const int32_t bank = f % kPipelineDepth;
        const int32_t base = bank * kFlightSize;

        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = ctx.commandPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkCheck(vkAllocateCommandBuffers(ctx.device, &cbai, &cmd),
                "vkAllocateCommandBuffers(flight)");
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(cmd, &cbbi), "vkBeginCommandBuffer(flight)");
        vkCmdResetQueryPool(cmd, tsPools[bank], 0, uint32_t(kFlightSize) * uint32_t(kTimestampsPerTile));

        int32_t count = 0;
        for (int32_t i = 0; i < kFlightSize && workRemaining(); ++i, ++count) {
            FlightSlot& s = slots[size_t(base + i)];
            if (pendingNext < pendingSlabs.size()) {
                // Remaining z-slab of the tall tile prepped just before.
                const ZWindow w = pendingSlabs[pendingNext++];
                std::vector<ZWindow> none;
                prepTileCpu(ctx, s, pendingTile, seed, tiles, amp, pendingVerify, stats, &w,
                            none);
            } else {
                const TileSpec& t = grid.tiles[tileCursor++];
                const int32_t linearIndex = t.ty * grid.numTiles + t.tx;
                const bool verify = fullVerify || (linearIndex % kVerifySampleStride == 0);
                pendingSlabs.clear();
                pendingNext = 0;
                prepTileCpu(ctx, s, t, seed, tiles, amp, verify, stats, nullptr, pendingSlabs);
                if (!pendingSlabs.empty()) {
                    ++tallTilesSplit;
                    extraSlabs += pendingSlabs.size();
                    pendingTile = t;
                    pendingVerify = verify;
                }
            }
            recordTileCommands(ctx, cmd, s, tsPools[bank], uint32_t(i) * uint32_t(kTimestampsPerTile));
        }
        for (int32_t i = count; i < kFlightSize; ++i) slots[size_t(base + i)].active = false;

        vkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(flight)");

        PendingFlight pf;
        pf.valid = true;
        pf.cmd = cmd;
        pf.bank = bank;
        pf.count = count;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCheck(vkCreateFence(ctx.device, &fci, nullptr, &pf.fence), "vkCreateFence(flight)");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &pf.cmd;
        vkCheck(vkQueueSubmit(ctx.queue, 1, &si, pf.fence), "vkQueueSubmit(flight)");
        return pf;
    };

    // Waits pf's fence (deferred: by the time this is called the GPU has
    // usually already finished, since the NEXT flight's CPU prep ran first
    // in the main loop below -- that's the double-buffering win), reads back
    // every tile's 7 GPU stage timings from pf's bank's query pool in one
    // call, then harvests (digest + CPU compare) every tile IN ORDER.
    const auto waitAndHarvest = [&](PendingFlight& pf) {
        vkCheck(vkWaitForFences(ctx.device, 1, &pf.fence, VK_TRUE, UINT64_MAX),
                "vkWaitForFences(flight)");
        vkDestroyFence(ctx.device, pf.fence, nullptr);
        pf.fence = VK_NULL_HANDLE;

        // Only request results for pf.count*kTimestampsPerTile queries -- NOT
        // the bank's full kFlightSize*kTimestampsPerTile capacity. On a
        // partial (last) flight, recordTileCommands() only wrote timestamps
        // for the first pf.count tiles; the remaining queries in the bank
        // were reset but never written, and VK_QUERY_RESULT_WAIT_BIT on an
        // unwritten query blocks forever (found via a hang on radius=16 and
        // radius=128, both of which have a partial last flight -- 9 tiles
        // and 529 tiles are not multiples of kFlightSize).
        std::vector<uint64_t> ts(size_t(pf.count) * size_t(kTimestampsPerTile), 0);
        vkCheck(vkGetQueryPoolResults(ctx.device, tsPools[pf.bank], 0, uint32_t(ts.size()),
                                       ts.size() * sizeof(uint64_t), ts.data(), sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                "vkGetQueryPoolResults(flight)");

        const int32_t base = pf.bank * kFlightSize;
        for (int32_t i = 0; i < pf.count; ++i) {
            const uint64_t* t = &ts[size_t(i) * size_t(kTimestampsPerTile)];
            stats.columnMs += double(t[1] - t[0]) * toMs;
            stats.voxelizeMs += double(t[2] - t[1]) * toMs;
            stats.meshCountMs += double(t[3] - t[2]) * toMs;
            stats.scanBlocksMs += double(t[4] - t[3]) * toMs;
            stats.scanSumsMs += double(t[5] - t[4]) * toMs;
            stats.scanAddMs += double(t[6] - t[5]) * toMs;
            stats.meshEmitMs += double(t[7] - t[6]) * toMs;
            harvestTile(slots[size_t(base + i)], stats);
        }
    };

    const auto tRunStart = Clock::now();
    PendingFlight prev;
    int32_t f = 0;
    while (workRemaining()) {
        PendingFlight cur = prepareAndSubmit(f); // may overlap flight f-1's still-pending fence
        if (prev.valid) waitAndHarvest(prev);    // deferred wait on flight f-1
        prev = cur;
        ++f;
        if (f % 8 == 0 || !workRemaining()) {
            std::printf("  flight %d done (%zu/%zu tiles consumed)\n", f, tileCursor, numTiles);
        }
    }
    if (prev.valid) waitAndHarvest(prev);
    if (tallTilesSplit) {
        std::printf(
            "  %zu tall tile(s) split into z-slabs (+%zu extra dispatch chains) to respect "
            "ScanSumsMain's 65,536-mask capacity (kMaxSlabInteriorLayers=%u)\n",
            tallTilesSplit, extraSlabs, kMaxSlabInteriorLayers);
    }
    const double wallMs = msSince(tRunStart);
    // Headline gate number: measured wall clock minus the two costs the gate
    // spec explicitly excludes. Computed once here (not summed per-tile/
    // per-flight) because flight pipelining makes per-tile windows overlap
    // in real time -- summing them would double-count the overlap.
    stats.gateMs = wallMs - stats.cpuColumnPassMs - stats.cpuCompareMs;

    // Snapshot BEFORE teardown: the cleanup loop below calls destroyBuffer on
    // every slot's buffers, which would otherwise fold post-gate frees into
    // the in-gate destroyOldMs figure.
    const AllocProfile allocProf = ctx.allocProfile;

    for (int32_t b = 0; b < kPipelineDepth; ++b) vkDestroyQueryPool(ctx.device, tsPools[b], nullptr);
    destroyFlightDescriptors(ctx, fd);
    size_t totalReallocs = 0;
    for (FlightSlot& s : slots) {
        totalReallocs += s.gb.totalReallocs();
        ctx.destroyBuffer(s.gb.paramsBuf);
        ctx.destroyBuffer(s.gb.columnsBuf);
        if (s.gb.elevBuf.buf.buffer) ctx.destroyBuffer(s.gb.elevBuf.buf);
        if (s.gb.climBuf.buf.buffer) ctx.destroyBuffer(s.gb.climBuf.buf);
        if (s.gb.cellsBuf.buf.buffer) ctx.destroyBuffer(s.gb.cellsBuf.buf);
        if (s.gb.countsBuf.buf.buffer) ctx.destroyBuffer(s.gb.countsBuf.buf);
        if (s.gb.offsetsBuf.buf.buffer) ctx.destroyBuffer(s.gb.offsetsBuf.buf);
        if (s.gb.blockSumsBuf.buf.buffer) ctx.destroyBuffer(s.gb.blockSumsBuf.buf);
        if (s.gb.quadsBuf.buf.buffer) ctx.destroyBuffer(s.gb.quadsBuf.buf);
    }

    const double gateSec = stats.gateMs / 1000.0;
    const double verifiedFraction =
        stats.totalTiles ? double(stats.verifiedTiles) / double(stats.totalTiles) : 0.0;
    const double scanMs = stats.scanBlocksMs + stats.scanSumsMs + stats.scanAddMs;
    const double gpuStagesMs =
        stats.columnMs + stats.voxelizeMs + stats.meshCountMs + scanMs + stats.meshEmitMs;
    // "As if serial" sum of every measured bucket -- with flight pipelining
    // this legitimately exceeds gateMs once CPU marshalling of one flight
    // hides inside the GPU busy time of another; the excess is time the
    // double-buffering actually hid, not a bug. Reported explicitly instead
    // of pretending the buckets are additive to gateMs (as the old fully
    // serial per-tile loop's remainder-based "marshalling overhead" line
    // implicitly did).
    const double serialEquivalentMs =
        stats.hostRasterFillMs + stats.marshallingMs + stats.bufferAllocMs + gpuStagesMs;
    const double pipeliningSavedMs =
        serialEquivalentMs > stats.gateMs ? serialEquivalentMs - stats.gateMs : 0.0;

    std::printf(
        "\n--- GATE stage totals (radius %lldm, %zu tiles, GPU busy-time from "
        "vkCmdWriteTimestamp queries) ---\n"
        "  columns dispatch:       %10.3f ms\n"
        "  voxelize dispatch:      %10.3f ms\n"
        "  mesh count dispatch:    %10.3f ms\n"
        "  GPU scan (blocks+sums+add): %10.3f ms  (blocks %.3f + sums %.3f + add %.3f)\n"
        "  mesh emit dispatch:     %10.3f ms\n"
        "  host raster fill:       %10.3f ms\n"
        "  marshalling (WorldGenParams fill+upload, persistent-descriptor writes on "
        "regrow only): %10.3f ms\n"
        "  buffer (re)allocation on grow (whole bracket): %10.3f ms\n"
        "      of which: free old (unmap+destroy+free) %8.3f | vkCreateBuffer+GetMemReq %8.3f | "
        "vkAllocateMemory %8.3f | vkBindBufferMemory %8.3f | vkMapMemory %8.3f ms\n"
        "      (%zu allocations, %.1f MiB total, host-visible memory type %u flags 0x%03x, "
        "heap %.0f MiB; the component figures cover EVERY createBuffer -- the regrows above plus "
        "the 2 fixed per-slot buffers (params, columns) built at slot setup -- so they sum to "
        "slightly more than the ensure()-only bracket total)\n"
        "  (sum of the above as if fully serial: %10.3f ms; hidden by flight double-"
        "buffering: %10.3f ms)\n"
        "  GATE TOTAL (measured wall clock, gpu path):  %10.3f ms  = %.3f s\n"
        "  CPU column pass (setup, excluded from gate):         %10.3f ms\n"
        "  CPU compare + digest (verification, excluded from gate): %10.3f ms\n"
        "  wall clock (everything, incl. both excluded costs):  %10.3f ms\n",
        (long long)radiusM, stats.totalTiles, stats.columnMs, stats.voxelizeMs,
        stats.meshCountMs, scanMs, stats.scanBlocksMs, stats.scanSumsMs, stats.scanAddMs,
        stats.meshEmitMs, stats.hostRasterFillMs, stats.marshallingMs, stats.bufferAllocMs,
        allocProf.destroyOldMs, allocProf.createBufferMs, allocProf.allocateMemoryMs,
        allocProf.bindMs, allocProf.mapMs, allocProf.allocCount,
        double(allocProf.bytesAllocated) / (1024.0 * 1024.0), allocProf.memTypeIndex,
        (unsigned)allocProf.memTypeFlags, double(allocProf.memHeapSize) / (1024.0 * 1024.0),
        serialEquivalentMs,
        pipeliningSavedMs, stats.gateMs, gateSec, stats.cpuColumnPassMs, stats.cpuCompareMs,
        wallMs);

    std::printf("\nGATE radius=%lldm: %.3f s (target: <1.000 s, plan sec4 M0) -> %s\n",
                (long long)radiusM, gateSec, gateSec < 1.0 ? "PASS" : "OVER TARGET");

    std::printf(
        "\nverified %zu/%zu tiles (%.1f%%): %zu/%zu columns, %zu/%zu cells, %zu/%zu quads "
        "compared; %zu interior bricks meshed total (all tiles, verified or not)\n",
        stats.verifiedTiles, stats.totalTiles, verifiedFraction * 100.0, stats.verifiedColumns,
        stats.totalColumns, stats.verifiedCells, stats.totalCells, stats.verifiedQuads,
        stats.totalQuads, stats.totalInteriorBricks);

    std::printf(
        "buffer reallocations across the run, all %d flight slots combined (elev/clim/cells/"
        "counts/offsets/quads): %zu (small and front-loaded -- each slot's buffers grow to a "
        "working-set size within its first few tiles, then every later tile reusing that slot "
        "reuses the buffer as-is)\n",
        totalSlots, totalReallocs);

    std::printf(
        "GATE output digest (columns + cells + quads, ALL gpu output, seed %llu): %016llx\n",
        (unsigned long long)seed, (unsigned long long)stats.digest.h);

    if (!stats.mismatches.empty()) {
        std::printf("\nFAIL: %zu mismatch(es) found in the verified sample (showing up to 20):\n",
                    stats.mismatches.size());
        for (const Mismatch& m : stats.mismatches) {
            std::printf("  (vx=%lld, vy=%lld) %s: cpu=%lld gpu=%lld\n", (long long)m.vx,
                        (long long)m.vy, m.field.c_str(), (long long)m.cpuVal,
                        (long long)m.gpuVal);
        }
        return 1;
    }

    std::printf("\nPASS: GPU output bit-exact with the CPU reference over the verified sample "
                "(radius %lldm)\n",
                (long long)radiusM);
    return 0;
}

} // namespace gate

} // namespace

int main(int argc, char** argv) {
    const uint64_t seed = 20260719; // vxc::SyntheticTileSampler seed, per task spec.
    std::string spvPath = VXC_SPV_PATH;
    std::string voxelizeSpvPath = VXC_SPV_PATH_VOXELIZE;
    std::string meshCountSpvPath = VXC_SPV_PATH_MESHCOUNT;
    std::string meshEmitSpvPath = VXC_SPV_PATH_MESHEMIT;
    std::string scanBlocksSpvPath = VXC_SPV_PATH_SCANBLOCKS;
    std::string scanSumsSpvPath = VXC_SPV_PATH_SCANSUMS;
    std::string scanAddSpvPath = VXC_SPV_PATH_SCANADD;
    int64_t gateRadiusM = -1; // -1 = not specified -> default regions mode (unchanged)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--spv" && i + 1 < argc) spvPath = argv[++i];
        else if (a == "--spv-voxelize" && i + 1 < argc) voxelizeSpvPath = argv[++i];
        else if (a == "--spv-meshcount" && i + 1 < argc) meshCountSpvPath = argv[++i];
        else if (a == "--spv-meshemit" && i + 1 < argc) meshEmitSpvPath = argv[++i];
        else if (a == "--spv-scanblocks" && i + 1 < argc) scanBlocksSpvPath = argv[++i];
        else if (a == "--spv-scansums" && i + 1 < argc) scanSumsSpvPath = argv[++i];
        else if (a == "--spv-scanadd" && i + 1 < argc) scanAddSpvPath = argv[++i];
        else if (a == "--radius" && i + 1 < argc) gateRadiusM = std::atoll(argv[++i]);
    }

    std::printf(
        "voxel-core GPU harness (ADR-0001 M0 gate) — ColumnMain + VoxelizeMain + "
        "MeshCount/EmitMain + GPU Scan(Blocks/Sums/Add)Main, seed %llu\n",
        (unsigned long long)seed);

    loadVulkanLoader();
    GpuContext ctx = createContext(spvPath, voxelizeSpvPath, meshCountSpvPath, meshEmitSpvPath,
                                    scanBlocksSpvPath, scanSumsSpvPath, scanAddSpvPath);
    std::printf("device: %s\n\n", ctx.deviceName.c_str());

    if (gateRadiusM >= 0) {
        const int rc = gate::runGateMode(ctx, gateRadiusM, seed);
        destroyContext(ctx);
        closeVulkanLoader();
        return rc;
    }

    // 64x64-column fixtures (8x8 bricks, 6x6 interior => 1,728 masks per
    // interior z-layer). 128x128 regions overflowed runMeshChain()'s 65,536-
    // mask cap once worldgen v3's spectral-gap octaves made relief tall
    // enough (origin hit 10 brick layers = 75,264 masks); at 64x64 the cap
    // needs > 37 interior layers (~30 m of relief inside a 6.4 m footprint)
    // — unreachable for surface terrain. Gate mode still exercises the full
    // 128x128 dispatch shape (with z-slab splitting, see ZWindow).
    // BOTH ORIGINAL REGIONS SIT AT NEGATIVE vx, and that turned out to matter.
    // Every hash in worldgen takes lattice/pixel indices derived from world
    // coordinates, so at negative coordinates it is fed negative arguments --
    // and a vendor that treats `>>` on a signed 64-bit value as a LOGICAL shift,
    // or that floors differently, diverges there and NOWHERE ELSE. With no
    // all-positive region in the gate, a divergence of that shape looks like
    // "everything is broken" and gives no way to localise it.
    //
    // "positive" is the control: same size, same code path, no negative
    // coordinate anywhere in it. If it passes while the other two fail, the
    // fault is coordinate-sign-dependent; if all three fail, it is not.
    //
    // THE FOURTH REGION EXERCISES THE OTHER OCTAVE TABLE (worldgen v10). The
    // three above all run at the 30 m pitch, which is the COARSE branch of
    // evalSurface's isFineTier test; without a fixture below 3750 mm the fine
    // ladder -- 4 octaves, 1 landform, its own channel-to-lattice mapping --
    // was mirrored into worldgen.ush and then never executed on the GPU by
    // anything. That is the same shape of hole as CoarseScale defaulting to 0
    // (see the params note in runRegion): the gate passes and tests nothing.
    //
    // 3750 mm is scale 8, the largest pitch isFineTier accepts, so it also pins
    // the THRESHOLD rather than merely landing somewhere inside the fine band --
    // an off-by-one in the comparison (`<` for `<=`) sends the CPU and the GPU
    // down different tables and shows up here as a total mismatch.
    //
    // It is deliberately smaller than the others. SyntheticTileSampler's octave
    // lattices are in PIXELS, so at an 8x finer pitch the same noise produces 8x
    // the world-space grade; a 64x64 footprint of that has enough vertical
    // relief to overrun runMeshChain()'s 65,536-mask cap. 24x24 columns is 3x3
    // bricks -- one interior brick, so the mesh path is still compared -- over a
    // 2.4 m footprint, which the cap comfortably covers.
    //
    // THE LAST TWO REGIONS REACH THE INTERIOR OF THE CURVATURE RAMP. On the
    // sampler's own raster the v10 gate is clamped in 100% of the columns above
    // (see ScaledTileSampler), so everything upstream of the gate is checked
    // only through a two-level step function. Dividing elevations by 100 puts
    // tier-normalised curvature at roughly -460 q10 under "origin" and +100..170
    // under "positive", against a knee of 512 -- inside the ramp on the CREST
    // side and the HOLLOW side respectively, so the gate's output becomes a
    // continuous function of the whole curvature chain and any magnitude error
    // in it shows up as a mismatch instead of clamping away.
    //
    // The hollow one also supplies the only inputs in the fixture where
    // `cScale - 1024` is negative AND ODD, which is the sole case in which
    // evalSurface's truncating `/ 2` on the micro band differs from a floor.
    const RegionSpec regions[] = {
        {"origin", -64, -64, 64, 64, 30000, 1},
        {"far-negative", -100000, 250000, 64, 64, 30000, 1},
        {"positive", 100064, 250064, 64, 64, 30000, 1},
        {"fine-tier", -12, 40008, 24, 24, 3750, 1},
        {"gentle-crest", -64, -64, 64, 64, 30000, 100},
        {"gentle-hollow", 100064, 250064, 64, 64, 30000, 100},
        // And one gentle FINE-tier region, which is the only fixture where
        // carrierCurvatureTierNormQ10 actually rounds. At 30000 its divide is
        // exact by construction (numerator is curvature * 30000^2 over 30000^2),
        // so floored and truncated division cannot differ there no matter what
        // the terrain does. At 3750 it divides by 64, and this origin puts all
        // 576 columns on the crest side with a numerator that is negative and
        // not a multiple of 64 -- i.e. 576 columns where the CPU's deliberate
        // floorDiv and a truncating divide give different answers.
        {"gentle-fine-crest", -400000, -400000, 24, 24, 3750, 100},
        // AND ONE REGION WHERE THE 3D DENSITY BAND ACTUALLY FIRES (v12).
        //
        // This one exists because of a near-miss. When the band was first wired
        // in at a 700 mm envelope, the six regions above happened to contain
        // gated columns and the cell comparison covered it. Halving the envelope
        // and promoting the lithology gate to the whole displacement made every
        // one of those regions non-rock or too gentle -- and vxc_gpu went on
        // PASSING, bit-exact, over the identical 739,328 cells and the identical
        // digest it produced at v11. A gate that closes everywhere in the
        // fixture is indistinguishable from a term that is not mirrored at all,
        // which is exactly the hole the fine-tier region above was added to
        // close for the fine octave table.
        //
        // So the fixture is chosen by SEARCH rather than by eye: this is the
        // 64x64 origin on the synthetic sampler with the highest gated-column
        // count and a relief small enough for runMeshChain's mask cap. All 4096
        // columns pass both gates and 956 of them are overhung, so every hash in
        // the band, the contrast curve, the taper and the per-column brick
        // widening are all live here -- and any divergence in them is a cell
        // mismatch rather than a silent pass.
        {"density-band-cliff", -8800, -30816, 64, 64, 30000, 1},
        // SAVANNA BOUNDARY (worldgen v22). Same argument as density-band-cliff
        // one paragraph up, for the gate v22 changed.
        //
        // v22 moved classifyBiome's savanna test from the seasonality byte
        // (bits 8-15 of the packed climate word) to the precipVariability byte
        // (bits 24-31), which means ColumnMain now blends a byte it never blended
        // before. NOT ONE of the eight fixtures above reaches that gate: over all
        // of their columns the synthetic sampler is either too cool or too wet,
        // so `warm && seasonal` is false everywhere and the branch is dead. That
        // was measured, not assumed -- a v21 CPU build compared against v22
        // SPIR-V reported 0 mismatches over 25,728 columns, i.e. the two
        // versions are indistinguishable on this fixture set. clPrecipVar could
        // have been shifted by the wrong amount and every region would still
        // have passed.
        //
        // Found by search (the same two-stage scan that produced the band-only
        // fixtures): the 2x2 tile-pixel block here STRADDLES the CV threshold
        // while all four corners stay warm and inside the savanna precipitation
        // window, and the origin is placed on the pixel corner, so the region
        // contains 2,133 SAVANNA columns and 1,963 TEMPERATE_FOREST ones with
        // the boundary between them set by the faded-bilinear blend of exactly
        // the byte that moved. A wrong shift, a wrong blend or a wrong threshold
        // relocates that boundary and shows up as column mismatches rather than
        // as a silent pass. Relief is 2.7 m, well inside runMeshChain's cap.
        //
        // Climate there (u8): temp 207, precip 17, bio_15 89 -- against gates
        // warm 185, arid 9, mod 34, CV 89.
        {"savanna-boundary", -249632, 1151968, 64, 64, 30000, 1},
    };

    Digest gpuDigest;
    std::vector<Mismatch> mismatches;
    constexpr size_t kMaxMismatchesPrinted = 20;
    // COUNT EVERY MISMATCH, print only the first few.
    //
    // The printed list is capped, and the cap used to be the only number
    // reported -- so a run whose first column disagreed filled the list with
    // that one column's fields and cells and reported "20 mismatch(es)"
    // whether the true count was 20 or 20,000. That is not a rounding error in
    // the diagnosis, it is the difference between "one bad corner" and "the
    // whole dispatch is wrong", and it cost real time to see through. Totals
    // are cheap; keep them separate from the sample.
    size_t totalMismatches = 0;
    size_t mismatchedColumns = 0;
    size_t totalColumns = 0;
    size_t totalCells = 0;
    size_t totalMeshedBricks = 0;
    size_t totalQuads = 0;
    double totalColumnDispatchMs = 0;
    double totalVoxelizeDispatchMs = 0;
    double totalMeshCountMs = 0;
    double totalMeshScanMs = 0;
    double totalMeshEmitMs = 0;

    for (const RegionSpec& region : regions) {
        const size_t regionMismatchesBefore = totalMismatches;
        const RegionResult gpu = runRegion(ctx, region, seed);
        totalColumnDispatchMs += gpu.columnDispatchMs;
        totalVoxelizeDispatchMs += gpu.voxelizeDispatchMs;
        std::printf("[%s] ColumnMain dispatch wall-clock: %.3f ms\n", region.name,
                    gpu.columnDispatchMs);
        std::printf("[%s] VoxelizeMain dispatch wall-clock: %.3f ms\n", region.name,
                    gpu.voxelizeDispatchMs);

        const uint32_t bricksX = region.width / 8;

        // --- ColumnMain field comparison (unchanged from the column-only
        // harness, just reusing gpu.cpuCols instead of recomputing it) -----
        for (uint32_t y = 0; y < region.height; ++y) {
            for (uint32_t x = 0; x < region.width; ++x) {
                const int64_t vx = int64_t(region.originVx) + x;
                const int64_t vy = int64_t(region.originVy) + y;
                const size_t colIdx = size_t(x) + size_t(y) * region.width;
                const GpuColumnSample& g = gpu.samples[colIdx];
                const ColumnSample& c = gpu.cpuCols[colIdx];
                ++totalColumns;

                gpuDigest.u32(static_cast<uint32_t>(g.surfaceMm));
                gpuDigest.u32(static_cast<uint32_t>(g.topsoilMm));
                gpuDigest.u32(static_cast<uint32_t>(g.subsoilMm));
                gpuDigest.u32(static_cast<uint32_t>(g.bedrockDepthMm));
                gpuDigest.u8(static_cast<uint8_t>(g.surfaceMat));

                size_t colMismatches = 0;
                auto record = [&](const char* field, int64_t cpuVal, int64_t gpuVal) {
                    if (cpuVal == gpuVal) return;
                    ++totalMismatches;
                    ++colMismatches;
                    if (mismatches.size() < kMaxMismatchesPrinted)
                        mismatches.push_back({vx, vy, field, cpuVal, gpuVal});
                };
                record("surfaceMm", c.surfaceMm, g.surfaceMm);
                record("topsoilMm", c.topsoilMm, g.topsoilMm);
                record("subsoilMm", c.subsoilMm, g.subsoilMm);
                record("bedrockDepthMm", c.bedrockDepthMm, g.bedrockDepthMm);
                record("surfaceMat", c.surfaceMat, g.surfaceMat);
                if (colMismatches > 0) ++mismatchedColumns;

                // --- VoxelizeMain cell comparison for this column's brick
                // stack (mirrors GeneratedWorld<8>::makeBrick's per-cell
                // Amplifier::materialAt call; the same column c is reused,
                // never recomputed). Layout matches worldgen.ush's
                // VoxelizeMain doc comment / cellIndexInBrick() above.
                const uint32_t bx = x / 8u, by = y / 8u, lx = x % 8u, ly = y % 8u;
                const uint32_t footprintIndex = bx + bricksX * by;
                for (uint32_t bz = 0; bz < gpu.bricksZ; ++bz) {
                    const size_t brickIndex = size_t(footprintIndex) * gpu.bricksZ + bz;
                    const int64_t brickZ = int64_t(gpu.brickZMin) + int64_t(bz);
                    for (uint32_t zLocal = 0; zLocal < 8u; ++zLocal) {
                        const int64_t vz = brickZ * 8 + zLocal;
                        const size_t cellIdx = brickIndex * 512 + cellIndexInBrick(lx, ly, zLocal);
                        const uint8_t cpuMat =
                            static_cast<uint8_t>(Amplifier::materialAt(c, vz));
                        const uint8_t gpuMat = static_cast<uint8_t>(gpu.cells[cellIdx] & 0xffu);
                        ++totalCells;
                        gpuDigest.u8(gpuMat);
                        if (cpuMat != gpuMat && mismatches.size() < kMaxMismatchesPrinted) {
                            mismatches.push_back({vx, vy, "cell@vz=" + std::to_string(vz),
                                                   cpuMat, gpuMat});
                        }
                    }
                }
            }
        }

        // --- Mesh comparison: CPU meshBrick<8> per interior brick vs the
        // GPU quad stream (docs/gpu-mesher-design.md ordering contract) ----
        if (gpu.interiorBricksZ > 0) {
            const double scanMs = gpu.meshScanBlocksDispatchMs + gpu.meshScanSumsDispatchMs +
                                   gpu.meshScanAddDispatchMs;
            std::printf("[%s] MeshCount dispatch: %.3f ms, GPU scan (blocks %.3f + sums %.3f + "
                        "add %.3f = %.3f) ms, MeshEmit dispatch: %.3f ms, "
                        "%u quads over %ux%ux%u interior bricks\n",
                        region.name, gpu.meshCountDispatchMs, gpu.meshScanBlocksDispatchMs,
                        gpu.meshScanSumsDispatchMs, gpu.meshScanAddDispatchMs, scanMs,
                        gpu.meshEmitDispatchMs, gpu.totalQuads, gpu.interiorBricksX,
                        gpu.interiorBricksY, gpu.interiorBricksZ);
            totalMeshCountMs += gpu.meshCountDispatchMs;
            totalMeshScanMs += scanMs;
            totalMeshEmitMs += gpu.meshEmitDispatchMs;

            std::vector<Quad> cpuQuads;
            size_t gpuCursor = 0;
            for (uint32_t iz = 0; iz < gpu.interiorBricksZ && mismatches.size() < kMaxMismatchesPrinted; ++iz) {
                for (uint32_t iy = 0; iy < gpu.interiorBricksY; ++iy) {
                    for (uint32_t ix = 0; ix < gpu.interiorBricksX; ++ix) {
                        // Region-voxel origin of this interior brick (+1 halo).
                        const int64_t ox = (int64_t(ix) + 1) * 8;
                        const int64_t oy = (int64_t(iy) + 1) * 8;
                        const int64_t oz = (int64_t(iz) + 1) * 8;
                        // Sampler on [-1,8]^3 brick-local coords via the CPU
                        // columns (same data VoxelizeMain consumed).
                        const auto sampler = [&](int x, int y, int z) -> MaterialId {
                            const int64_t rvx = ox + x, rvy = oy + y;
                            const int64_t vz =
                                (int64_t(gpu.brickZMin)) * 8 + oz + z;
                            const ColumnSample& c =
                                gpu.cpuCols[size_t(rvx) + size_t(rvy) * region.width];
                            return Amplifier::materialAt(c, vz);
                        };
                        cpuQuads.clear();
                        meshBrick<8>(sampler, cpuQuads);
                        ++totalMeshedBricks;
                        totalQuads += cpuQuads.size();

                        for (const Quad& q : cpuQuads) {
                            uint64_t gq = gpuCursor < gpu.quads.size() ? gpu.quads[gpuCursor] : ~0ull;
                            const uint32_t w0 = uint32_t(gq & 0xffffffffu);
                            const uint32_t w1 = uint32_t(gq >> 32);
                            const uint8_t gAxis = w0 & 0xfu, gDir = (w0 >> 4) & 0xfu,
                                          gSlice = (w0 >> 8) & 0xffu, gU0 = (w0 >> 16) & 0xffu,
                                          gV0 = (w0 >> 24) & 0xffu;
                            const uint8_t gW = w1 & 0xffu, gH = (w1 >> 8) & 0xffu,
                                          gAo = (w1 >> 16) & 0xffu, gMat = (w1 >> 24) & 0xffu;
                            gpuDigest.u8(gAxis);
                            gpuDigest.u8(gDir);
                            gpuDigest.u8(gSlice);
                            gpuDigest.u8(gU0);
                            gpuDigest.u8(gV0);
                            gpuDigest.u8(gW);
                            gpuDigest.u8(gH);
                            gpuDigest.u8(gAo);
                            gpuDigest.u8(gMat);
                            const bool same = gAxis == q.axis && gDir == q.positive &&
                                              gSlice == q.slice && gU0 == q.u0 && gV0 == q.v0 &&
                                              gW == q.w && gH == q.h && gAo == q.ao && gMat == q.mat;
                            if (!same && mismatches.size() < kMaxMismatchesPrinted) {
                                mismatches.push_back(
                                    {int64_t(ix), int64_t(iy),
                                     "quad@brick(" + std::to_string(ix) + "," +
                                         std::to_string(iy) + "," + std::to_string(iz) +
                                         ")#" + std::to_string(gpuCursor),
                                     int64_t(q.mat), int64_t(gMat)});
                            }
                            ++gpuCursor;
                        }
                    }
                }
            }
            if (gpuCursor != gpu.quads.size() && mismatches.size() < kMaxMismatchesPrinted) {
                mismatches.push_back({0, 0,
                                       "quadCount total (cpu vs gpu)", int64_t(gpuCursor),
                                       int64_t(gpu.quads.size())});
            }
        }

        // PER-REGION verdict. A single global total cannot distinguish "one
        // region is broken" from "everything is", and that distinction is the
        // whole reason the positive-coordinate control region exists.
        std::printf("[%s] mismatches in this region: %zu\n", region.name,
                    totalMismatches - regionMismatchesBefore);
        // First column of the region, field by field. A per-region sample is
        // what distinguishes "the same wrong constant everywhere" (a hash fed
        // the wrong coordinates) from "wrong by a little, differently each
        // time" (an arithmetic divergence).
        std::printf("[%s]   col0 vx=%lld vy=%lld  surfaceMm cpu=%d gpu=%d  "
                    "bedrock cpu=%d gpu=%d  mat cpu=%d gpu=%d\n",
                    region.name, (long long)region.originVx, (long long)region.originVy,
                    gpu.cpuCols[0].surfaceMm, gpu.samples[0].surfaceMm,
                    gpu.cpuCols[0].bedrockDepthMm, gpu.samples[0].bedrockDepthMm,
                    (int)gpu.cpuCols[0].surfaceMat, (int)gpu.samples[0].surfaceMat);
    }

    const std::string deviceName = ctx.deviceName;
    destroyContext(ctx);
    closeVulkanLoader();

    const double voxelizeSec = totalVoxelizeDispatchMs / 1000.0;
    const double cellsPerSec = voxelizeSec > 0.0 ? double(totalCells) / voxelizeSec : 0.0;

    const double meshSec = (totalMeshCountMs + totalMeshScanMs + totalMeshEmitMs) / 1000.0;
    const double quadsPerSec = meshSec > 0.0 ? double(totalQuads) / meshSec : 0.0;
    std::printf(
        "\ncompared %zu columns, %zu cells, %zu meshed bricks (%zu quads) across %zu regions\n"
        "  ColumnMain total dispatch wall-clock:   %.3f ms\n"
        "  VoxelizeMain total dispatch wall-clock: %.3f ms  (%.3f Mcells/sec)\n"
        "  Mesh count+GPUscan+emit total wall-clock: %.3f ms  (count %.3f + scan %.3f + emit "
        "%.3f)  (%.3f Mquads/sec)\n",
        totalColumns, totalCells, totalMeshedBricks, totalQuads,
        sizeof(regions) / sizeof(regions[0]), totalColumnDispatchMs, totalVoxelizeDispatchMs,
        cellsPerSec / 1e6, totalMeshCountMs + totalMeshScanMs + totalMeshEmitMs, totalMeshCountMs,
        totalMeshScanMs, totalMeshEmitMs, quadsPerSec / 1e6);
    std::printf("GPU output digest (columns + cells + quads, combined): %016llx\n",
                (unsigned long long)gpuDigest.h);

    if (!mismatches.empty()) {
        std::printf("\nFAIL: %zu column-field mismatch(es) over %zu of %zu columns "
                    "(%.2f%%); showing the first %zu of all kinds:\n",
                    totalMismatches, mismatchedColumns, totalColumns,
                    totalColumns > 0 ? 100.0 * double(mismatchedColumns) / double(totalColumns)
                                     : 0.0,
                    kMaxMismatchesPrinted);
        for (const Mismatch& m : mismatches) {
            std::printf("  (vx=%lld, vy=%lld) %s: cpu=%lld gpu=%lld\n", (long long)m.vx,
                        (long long)m.vy, m.field.c_str(), (long long)m.cpuVal,
                        (long long)m.gpuVal);
        }
        std::printf("\nFAIL: GPU output does not bit-exactly match the CPU reference\n");
        return 1;
    }

    std::printf(
        "\nPASS: GPU output is bit-exact with the CPU reference (%s) over %zu columns, %zu "
        "cells, %zu quads\n",
        deviceName.c_str(), totalColumns, totalCells, totalQuads);
    return 0;
}
