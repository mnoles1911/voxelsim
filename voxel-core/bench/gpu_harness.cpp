// M0-gate Vulkan harness (ADR-0001): dispatches the SPIR-V worldgen kernel
// (voxel-core/shaders/worldgen.hlsl, ColumnMain) on this machine's GPU and
// byte-compares every field of every output column against the CPU
// reference (vxc::Amplifier::column). This desktop is the ADR's AMD leg
// (RX 7800 XT) of the NVIDIA-vs-AMD M0 determinism gate.
//
// Bench code: wall-clock timing uses floats/doubles, but every value fed to
// or read from the shader is integer, matching docs/determinism.md and the
// float-free contract of voxel-core/src and voxel-core/include.
//
// No Vulkan SDK dependency: vulkan-1.dll is loaded dynamically at runtime
// (LoadLibraryA + vkGetInstanceProcAddr) and every Vulkan entry point used
// below is resolved through that one function via the X-macro tables, so
// there is no import-lib link dependency — only tools/vulkan-headers'
// headers (types/structs/PFN_ typedefs) are needed at compile time.
//
// Resource bindings mirror worldgen.hlsl exactly, but DXC's default SPIR-V
// codegen maps each HLSL register class (b/t/u) independently onto the same
// flat Vulkan (set, binding) space, so b0/t0/u0 collide at (0,0) unless
// shifted. tools/compile-shaders.ps1 compiles with -fvk-t-shift 1 0
// -fvk-u-shift 3 0, producing the layout hardcoded here:
//   binding 0: WorldGenParams   (VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b0)
//   binding 1: ElevationMm      (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, t0)
//   binding 2: ClimatePacked    (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, t1)
//   binding 3: OutColumns       (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, u0, RW)

#ifndef _WIN32
#error "gpu_harness.cpp is Windows-only for now (ADR-0001 M0 scope)"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// Headless compute only — no WSI/surface, so plain vulkan_core.h (no
// platform header) is enough. VK_NO_PROTOTYPES keeps this a header-only
// dependency: no vulkan-1.lib, every entry point resolved dynamically below.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

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
#include "voxelcore/tiles.h"

using namespace vxc;

namespace {

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
    X(vkQueueSubmit)

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

HMODULE g_vulkanDll = nullptr;

void loadVulkanLoader() {
    g_vulkanDll = LoadLibraryA("vulkan-1.dll");
    if (!g_vulkanDll) fail("vulkan-1.dll not found (no Vulkan runtime installed)");
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g_vulkanDll, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) fail("vulkan-1.dll missing vkGetInstanceProcAddr export");

#define VXC_LOAD_GLOBAL(name)                                                          \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));        \
    if (!name) fail("missing global Vulkan entry point " #name);
    VXC_VK_GLOBAL_FUNCS(VXC_LOAD_GLOBAL)
#undef VXC_LOAD_GLOBAL
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

// GPU-side mirror of worldgen.hlsl's GpuColumnSample (5 x 4 bytes, tightly
// packed — verified against the compiled SPIR-V's member offsets).
struct GpuColumnSample {
    int32_t surfaceMm;
    int32_t topsoilMm;
    int32_t subsoilMm;
    int32_t bedrockDepthMm;
    uint32_t surfaceMat;
};
static_assert(sizeof(GpuColumnSample) == 20, "GpuColumnSample must match the HLSL layout");

// GPU-side mirror of worldgen.hlsl's cbuffer WorldGenParams (48 bytes,
// tightly packed — verified against the compiled SPIR-V's member offsets;
// nothing here straddles a 16-byte boundary so DXC's HLSL-style cbuffer
// packing matches plain sequential layout).
struct WorldGenParamsCB {
    uint32_t DispatchColumnsX, DispatchColumnsY;
    int32_t RasterOriginPxX, RasterOriginPxY;
    uint32_t RasterSizeX, RasterSizeY;
    int32_t PixelSizeMm;
    uint32_t SeedLo;
    uint32_t SeedHi;
    int32_t OriginVx;
    int32_t OriginVy;
    int32_t Pad0;
};
static_assert(sizeof(WorldGenParamsCB) == 48, "WorldGenParamsCB must match the HLSL cbuffer layout");

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
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
    std::string deviceName;

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b;
        b.size = size;
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
        vkCheck(vkAllocateMemory(device, &mai, nullptr, &b.memory), "vkAllocateMemory");
        vkCheck(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
        vkCheck(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped), "vkMapMemory");
        return b;
    }

    void destroyBuffer(Buffer& b) {
        if (b.mapped) vkUnmapMemory(device, b.memory);
        if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device, b.memory, nullptr);
        b = Buffer{};
    }
};

GpuContext createContext(const std::string& spvPath) {
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
             ") does not support shaderInt64 — required by worldgen.hlsl's 64-bit hash "
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

    return ctx;
}

void destroyContext(GpuContext& ctx) {
    if (ctx.device) {
        // vkDestroyDescriptorPool implicitly frees ctx.descSet.
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

struct RegionSpec {
    const char* name;
    int32_t originVx, originVy;
    uint32_t width, height; // dispatch columns
};

struct RegionResult {
    std::vector<GpuColumnSample> samples; // width*height, row-major (x fast)
    double dispatchMs = 0; // wall-clock: submit -> fence signalled
};

RegionResult runRegion(GpuContext& ctx, const RegionSpec& region, uint64_t seed) {
    SyntheticTileSampler tiles(seed);
    const int64_t pixelSizeMm = tiles.pixelSizeMm();

    // Raster window covering every bilinear tap the dispatch touches
    // (worldgen.hlsl's documented contract): pixel range from the column mm
    // range, +1 on the high end for the second bilinear tap.
    const int64_t xMmMin = int64_t(region.originVx) * kVoxelSizeMm;
    const int64_t xMmMax = int64_t(region.originVx + int32_t(region.width) - 1) * kVoxelSizeMm;
    const int64_t yMmMin = int64_t(region.originVy) * kVoxelSizeMm;
    const int64_t yMmMax = int64_t(region.originVy + int32_t(region.height) - 1) * kVoxelSizeMm;
    const int64_t pxMin = floorDiv(xMmMin, pixelSizeMm);
    const int64_t pxMax = floorDiv(xMmMax, pixelSizeMm) + 1;
    const int64_t pyMin = floorDiv(yMmMin, pixelSizeMm);
    const int64_t pyMax = floorDiv(yMmMax, pixelSizeMm) + 1;
    const uint32_t rasterW = static_cast<uint32_t>(pxMax - pxMin + 1);
    const uint32_t rasterH = static_cast<uint32_t>(pyMax - pyMin + 1);

    std::printf("[%s] origin (%d,%d) %ux%u columns, raster window (%lld,%lld) %ux%u px\n",
                region.name, region.originVx, region.originVy, region.width, region.height,
                (long long)pxMin, (long long)pyMin, rasterW, rasterH);

    Buffer paramsBuf = ctx.createBuffer(sizeof(WorldGenParamsCB),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    Buffer elevBuf = ctx.createBuffer(VkDeviceSize(rasterW) * rasterH * sizeof(int32_t),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer climBuf = ctx.createBuffer(VkDeviceSize(rasterW) * rasterH * sizeof(uint32_t),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer outBuf = ctx.createBuffer(
        VkDeviceSize(region.width) * region.height * sizeof(GpuColumnSample),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // Fill the raster window exactly as Amplifier::column would read it
    // through the same SyntheticTileSampler (elevation mm, packed climate
    // t | s<<8 | p<<16 | v<<24 — matches worldgen.hlsl's cl unpack).
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
    params.Pad0 = 0;
    std::memcpy(paramsBuf.mapped, &params, sizeof(params));

    VkDescriptorBufferInfo paramsInfo{paramsBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo elevInfo{elevBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo climInfo{climBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo outInfo{outBuf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[4]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.descSet, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &paramsInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.descSet, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &elevInfo, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.descSet, 2, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &climInfo, nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.descSet, 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &outInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 4, writes, 0, nullptr);

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = ctx.commandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(ctx.device, &cbai, &cmd), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(cmd, &cbbi), "vkBeginCommandBuffer");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelineLayout, 0, 1,
                             &ctx.descSet, 0, nullptr);
    const uint32_t groupsX = (region.width + 7) / 8;
    const uint32_t groupsY = (region.height + 7) / 8;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Visibility barrier: SHADER_WRITE -> HOST_READ on the output buffer
    // before the host maps it post-fence (HOST_COHERENT memory skips the
    // flush/invalidate calls, not the Vulkan memory-domain visibility op).
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = outBuf.buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                          0, nullptr, 1, &barrier, 0, nullptr);
    vkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCheck(vkCreateFence(ctx.device, &fci, nullptr, &fence), "vkCreateFence");

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    const auto tDispatch = Clock::now();
    vkCheck(vkQueueSubmit(ctx.queue, 1, &submit, fence), "vkQueueSubmit");
    vkCheck(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    const double dispatchMs = msSince(tDispatch);

    vkDestroyFence(ctx.device, fence, nullptr);
    // Command buffer is reclaimed when ctx.commandPool is destroyed; no
    // explicit vkFreeCommandBuffers needed for this one-shot-per-region use.

    RegionResult result;
    result.dispatchMs = dispatchMs;
    result.samples.resize(size_t(region.width) * region.height);
    std::memcpy(result.samples.data(), outBuf.mapped,
                result.samples.size() * sizeof(GpuColumnSample));

    ctx.destroyBuffer(paramsBuf);
    ctx.destroyBuffer(elevBuf);
    ctx.destroyBuffer(climBuf);
    ctx.destroyBuffer(outBuf);

    return result;
}

// --- comparison against the CPU reference -----------------------------------

struct Mismatch {
    int64_t vx, vy;
    std::string field;
    int64_t cpuVal, gpuVal;
};

} // namespace

int main(int argc, char** argv) {
    const uint64_t seed = 20260719; // vxc::SyntheticTileSampler seed, per task spec.
    std::string spvPath = VXC_SPV_PATH;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--spv" && i + 1 < argc) spvPath = argv[++i];
    }

    std::printf("voxel-core GPU harness (ADR-0001 M0 gate) — worldgen.ColumnMain, seed %llu\n",
                (unsigned long long)seed);
    std::printf("shader: %s\n", spvPath.c_str());

    loadVulkanLoader();
    GpuContext ctx = createContext(spvPath);
    std::printf("device: %s\n\n", ctx.deviceName.c_str());

    const RegionSpec regions[] = {
        {"origin", -64, -64, 128, 128},
        {"far-negative", -100000, 250000, 128, 128},
    };

    Digest gpuDigest;
    std::vector<Mismatch> mismatches;
    constexpr size_t kMaxMismatchesPrinted = 20;
    size_t totalColumns = 0;
    double totalDispatchMs = 0;

    for (const RegionSpec& region : regions) {
        const RegionResult gpu = runRegion(ctx, region, seed);
        totalDispatchMs += gpu.dispatchMs;
        std::printf("[%s] dispatch wall-clock: %.3f ms\n", region.name, gpu.dispatchMs);

        SyntheticTileSampler cpuTiles(seed);
        Amplifier cpuAmp(seed, cpuTiles);

        for (uint32_t y = 0; y < region.height; ++y) {
            for (uint32_t x = 0; x < region.width; ++x) {
                const int64_t vx = int64_t(region.originVx) + x;
                const int64_t vy = int64_t(region.originVy) + y;
                const GpuColumnSample& g = gpu.samples[size_t(x) + size_t(y) * region.width];
                const ColumnSample c = cpuAmp.column(vx, vy);
                ++totalColumns;

                gpuDigest.u32(static_cast<uint32_t>(g.surfaceMm));
                gpuDigest.u32(static_cast<uint32_t>(g.topsoilMm));
                gpuDigest.u32(static_cast<uint32_t>(g.subsoilMm));
                gpuDigest.u32(static_cast<uint32_t>(g.bedrockDepthMm));
                gpuDigest.u8(static_cast<uint8_t>(g.surfaceMat));

                auto record = [&](const char* field, int64_t cpuVal, int64_t gpuVal) {
                    if (cpuVal != gpuVal && mismatches.size() < kMaxMismatchesPrinted)
                        mismatches.push_back({vx, vy, field, cpuVal, gpuVal});
                };
                record("surfaceMm", c.surfaceMm, g.surfaceMm);
                record("topsoilMm", c.topsoilMm, g.topsoilMm);
                record("subsoilMm", c.subsoilMm, g.subsoilMm);
                record("bedrockDepthMm", c.bedrockDepthMm, g.bedrockDepthMm);
                record("surfaceMat", c.surfaceMat, g.surfaceMat);
            }
        }
    }

    const std::string deviceName = ctx.deviceName;
    destroyContext(ctx);
    FreeLibrary(g_vulkanDll);

    std::printf("\ncompared %zu columns across %zu regions, total dispatch wall-clock %.3f ms\n",
                totalColumns, sizeof(regions) / sizeof(regions[0]), totalDispatchMs);
    std::printf("GPU output digest: %016llx\n", (unsigned long long)gpuDigest.h);

    if (!mismatches.empty()) {
        std::printf("\nFAIL: %zu mismatch(es) (showing up to %zu):\n", mismatches.size(),
                    kMaxMismatchesPrinted);
        for (const Mismatch& m : mismatches) {
            std::printf("  (vx=%lld, vy=%lld) %s: cpu=%lld gpu=%lld\n", (long long)m.vx,
                        (long long)m.vy, m.field.c_str(), (long long)m.cpuVal,
                        (long long)m.gpuVal);
        }
        std::printf("\nFAIL: GPU output does not bit-exactly match the CPU reference\n");
        return 1;
    }

    std::printf("\nPASS: GPU output is bit-exact with the CPU reference (%s) over %zu columns\n",
                deviceName.c_str(), totalColumns);
    return 0;
}
