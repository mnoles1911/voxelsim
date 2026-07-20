// M0-gate Vulkan harness (ADR-0001): dispatches the SPIR-V worldgen kernels
// (voxel-core/shaders/worldgen.hlsl, ColumnMain + VoxelizeMain) on this
// machine's GPU and byte-compares every field/cell against the CPU reference
// (vxc::Amplifier::column / vxc::Amplifier::materialAt). This desktop is the
// ADR's AMD leg (RX 7800 XT) of the NVIDIA-vs-AMD M0 determinism gate.
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
// Two kernels, two pipelines, one shared cbuffer, chained through one
// buffer: ColumnMain writes OutColumns; that SAME VkBuffer is then bound as
// VoxelizeMain's InColumns (a pipeline barrier makes the write visible to
// the second dispatch's reads) — columns are never recomputed on GPU.
//
// Resource bindings mirror worldgen.hlsl exactly, but DXC's default SPIR-V
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
#include "voxelcore/mesher.h"
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

// GPU-side mirror of worldgen.hlsl's cbuffer WorldGenParams (52 bytes,
// tightly packed — verified against the compiled SPIR-V's member offsets;
// nothing here straddles a 16-byte boundary so DXC's HLSL-style cbuffer
// packing matches plain sequential layout). BrickZMin/BricksZ are read only
// by VoxelizeMain; the rest is shared or ColumnMain-only (see worldgen.hlsl).
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
};
static_assert(sizeof(WorldGenParamsCB) == 52, "WorldGenParamsCB must match the HLSL cbuffer layout");

// Cell index within one 8^3 brick — mirrors vxc::Brick<8>::cellIndex AND
// worldgen.hlsl's cellIndexInBrick exactly.
constexpr uint32_t cellIndexInBrick(uint32_t x, uint32_t y, uint32_t z) {
    return x + 8u * (y + 8u * z);
}

// OutCells (RWStructuredBuffer<uint>, one uint per cell, material id 0-255 in
// the low byte) layout — mirrors worldgen.hlsl's VoxelizeMain doc comment
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

    // Mesh kernels (MeshCountMain / MeshEmitMain) share ONE superset
    // descriptor layout (binding 0 uniform; 5 cells, 6 offsets, 7 counts,
    // 8 quads storage) — a layout binding a kernel's SPIR-V doesn't declare
    // is legal, and it lets both kernels reuse identical set wiring.
    VkDescriptorSetLayout meshDescSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout meshPipelineLayout = VK_NULL_HANDLE;
    VkShaderModule meshCountModule = VK_NULL_HANDLE;
    VkShaderModule meshEmitModule = VK_NULL_HANDLE;
    VkPipeline meshCountPipeline = VK_NULL_HANDLE;
    VkPipeline meshEmitPipeline = VK_NULL_HANDLE;
    VkDescriptorPool meshDescPool = VK_NULL_HANDLE;
    VkDescriptorSet meshCountSet = VK_NULL_HANDLE;
    VkDescriptorSet meshEmitSet = VK_NULL_HANDLE;

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

GpuContext createContext(const std::string& spvPath, const std::string& voxelizeSpvPath,
                          const std::string& meshCountSpvPath,
                          const std::string& meshEmitSpvPath) {
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

    // --- VoxelizeMain pipeline: separate SPIR-V module, separate descriptor
    // set layout (binding 0 uniform WorldGenParams shared with ColumnMain's
    // buffer, binding 4 InColumns read-only storage, binding 5 OutCells RW
    // storage — see the file header for the DXC -fvk-*-shift derivation).
    VkDescriptorSetLayoutBinding voxBindings[3]{};
    voxBindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    voxBindings[1] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    voxBindings[2] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo voxDslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    voxDslci.bindingCount = 3;
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
    voxPoolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
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

    // --- Mesh pipelines: MeshCountMain + MeshEmitMain, shared superset
    // layout (see GpuContext comment), two descriptor sets with identical
    // buffer wiring so count/emit only differ by pipeline bind.
    VkDescriptorSetLayoutBinding meshBindings[5]{};
    meshBindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    meshBindings[1] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    meshBindings[2] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    meshBindings[3] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    meshBindings[4] = {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo meshDslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    meshDslci.bindingCount = 5;
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

    VkDescriptorPoolSize meshPoolSizes[2]{};
    meshPoolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2};
    meshPoolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8};
    VkDescriptorPoolCreateInfo meshDpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    meshDpci.maxSets = 2;
    meshDpci.poolSizeCount = 2;
    meshDpci.pPoolSizes = meshPoolSizes;
    vkCheck(vkCreateDescriptorPool(ctx.device, &meshDpci, nullptr, &ctx.meshDescPool),
            "vkCreateDescriptorPool(mesh)");

    VkDescriptorSetLayout meshLayouts[2] = {ctx.meshDescSetLayout, ctx.meshDescSetLayout};
    VkDescriptorSet meshSets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorSetAllocateInfo meshDsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    meshDsai.descriptorPool = ctx.meshDescPool;
    meshDsai.descriptorSetCount = 2;
    meshDsai.pSetLayouts = meshLayouts;
    vkCheck(vkAllocateDescriptorSets(ctx.device, &meshDsai, meshSets),
            "vkAllocateDescriptorSets(mesh)");
    ctx.meshCountSet = meshSets[0];
    ctx.meshEmitSet = meshSets[1];

    return ctx;
}

void destroyContext(GpuContext& ctx) {
    if (ctx.device) {
        if (ctx.meshDescPool) vkDestroyDescriptorPool(ctx.device, ctx.meshDescPool, nullptr);
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

struct RegionSpec {
    const char* name;
    int32_t originVx, originVy;
    uint32_t width, height; // dispatch columns
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
    // deterministic count->scan->emit order. quadOffsets has maskCount+1
    // entries (exclusive scan + total).
    std::vector<uint64_t> quads;       // packed (word0 | word1<<32)
    std::vector<uint32_t> quadOffsets; // per-mask exclusive offsets + total tail
    uint32_t interiorBricksX = 0, interiorBricksY = 0, interiorBricksZ = 0;
    double meshCountDispatchMs = 0;
    double meshEmitDispatchMs = 0;
};

RegionResult runRegion(GpuContext& ctx, const RegionSpec& region, uint64_t seed) {
    if (region.width % 8 != 0 || region.height % 8 != 0) {
        fail(std::string("region '") + region.name +
             "' dispatch footprint must be brick-aligned (width/height multiples of 8) "
             "per worldgen.hlsl's VoxelizeMain contract");
    }

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

    // CPU columns for the WHOLE region, computed once up front. This serves
    // two purposes and both need it before the GPU dispatch: (a) it derives
    // VoxelizeMain's z-range the same way GeneratedWorld<8>::surfaceBrickRange
    // does (top-voxel min/max -> brick z range), except over the whole region
    // rather than a single 8x8 footprint, since BrickZMin/BricksZ are one
    // pair of scalars shared by every footprint in the dispatch; (b) it is
    // returned to the caller and reused as ground truth for BOTH the
    // ColumnMain field comparison and the VoxelizeMain cell comparison, so
    // vxc::Amplifier::column is never computed twice for the same column.
    SyntheticTileSampler cpuTiles(seed);
    Amplifier cpuAmp(seed, cpuTiles);
    std::vector<ColumnSample> cpuCols(size_t(region.width) * region.height);
    int64_t vzMin = INT64_MAX, vzMax = INT64_MIN;
    for (uint32_t y = 0; y < region.height; ++y) {
        for (uint32_t x = 0; x < region.width; ++x) {
            const int64_t vx = int64_t(region.originVx) + x;
            const int64_t vy = int64_t(region.originVy) + y;
            const ColumnSample c = cpuAmp.column(vx, vy);
            cpuCols[size_t(x) + size_t(y) * region.width] = c;
            // Topmost solid voxel: centre (vz*100+50) <= surfaceMm (mirrors
            // GeneratedWorld<8>::surfaceBrickRange).
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
    params.BrickZMin = brickZMin;
    params.BricksZ = bricksZ;
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
    // binding 4 (InColumns) is the SAME columnsBuf ColumnMain just wrote,
    // binding 5 (OutCells) is the new cellsBuf.
    VkDescriptorBufferInfo cellsInfo{cellsBuf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet voxWrites[3]{};
    voxWrites[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.voxDescSet, 0, 0, 1,
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &paramsInfo, nullptr};
    voxWrites[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.voxDescSet, 4, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &columnsInfo, nullptr};
    voxWrites[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ctx.voxDescSet, 5, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &cellsInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 3, voxWrites, 0, nullptr);

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

    // --- Passes 3+4: MeshCountMain -> host exclusive scan -> MeshEmitMain --
    // (docs/gpu-mesher-design.md: deterministic ordering with no atomics).
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
        // Placeholder quads buffer for the count pass (the superset layout
        // requires a valid buffer at binding 8 even though count never
        // touches it); replaced by the real one for emit.
        Buffer quadsStub = ctx.createBuffer(sizeof(uint64_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        const auto writeMeshSet = [&](VkDescriptorSet set, VkBuffer quadsBuffer) {
            VkDescriptorBufferInfo pInfo{paramsBuf.buffer, 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo cInfo{cellsBuf.buffer, 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo oInfo{offsetsBuf.buffer, 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo nInfo{countsBuf.buffer, 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo qInfo{quadsBuffer, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w[5]{};
            w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &pInfo, nullptr};
            w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 5, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &cInfo, nullptr};
            w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 6, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &oInfo, nullptr};
            w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 7, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &nInfo, nullptr};
            w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 8, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &qInfo, nullptr};
            vkUpdateDescriptorSets(ctx.device, 5, w, 0, nullptr);
        };

        const uint32_t meshGroups = (maskCount + 63) / 64;
        const auto runMeshPass = [&](VkPipeline pipeline, VkDescriptorSet set, Buffer& hostReadBuf,
                                      const char* what) -> double {
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            vkCheck(vkAllocateCommandBuffers(ctx.device, &cbai, &cmd), what);
            vkCheck(vkBeginCommandBuffer(cmd, &cbbi), what);
            // Visibility for this submission's reads of the voxelize output.
            VkBufferMemoryBarrier readBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            readBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            readBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            readBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            readBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            readBarrier.buffer = cellsBuf.buffer;
            readBarrier.offset = 0;
            readBarrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                  &readBarrier, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.meshPipelineLayout,
                                     0, 1, &set, 0, nullptr);
            vkCmdDispatch(cmd, meshGroups, 1, 1);
            VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            hostBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostBarrier.buffer = hostReadBuf.buffer;
            hostBarrier.offset = 0;
            hostBarrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostBarrier, 0,
                                  nullptr);
            vkCheck(vkEndCommandBuffer(cmd), what);
            VkFence fence = VK_NULL_HANDLE;
            vkCheck(vkCreateFence(ctx.device, &fci, nullptr, &fence), what);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            const auto t0 = Clock::now();
            vkCheck(vkQueueSubmit(ctx.queue, 1, &si, fence), what);
            vkCheck(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX), what);
            const double ms = msSince(t0);
            vkDestroyFence(ctx.device, fence, nullptr);
            return ms;
        };

        writeMeshSet(ctx.meshCountSet, quadsStub.buffer);
        result.meshCountDispatchMs = runMeshPass(ctx.meshCountPipeline, ctx.meshCountSet,
                                                  countsBuf, "mesh-count");

        // Host exclusive scan (deterministic by definition).
        const uint32_t* counts = static_cast<const uint32_t*>(countsBuf.mapped);
        result.quadOffsets.resize(size_t(maskCount) + 1);
        uint32_t running = 0;
        for (uint32_t i = 0; i < maskCount; ++i) {
            result.quadOffsets[i] = running;
            running += counts[i];
        }
        result.quadOffsets[maskCount] = running;
        std::memcpy(offsetsBuf.mapped, result.quadOffsets.data(),
                    size_t(maskCount) * sizeof(uint32_t));

        if (running > 0) {
            Buffer quadsBuf = ctx.createBuffer(VkDeviceSize(running) * sizeof(uint64_t),
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            writeMeshSet(ctx.meshEmitSet, quadsBuf.buffer);
            result.meshEmitDispatchMs =
                runMeshPass(ctx.meshEmitPipeline, ctx.meshEmitSet, quadsBuf, "mesh-emit");
            result.quads.resize(running);
            std::memcpy(result.quads.data(), quadsBuf.mapped, size_t(running) * sizeof(uint64_t));
            ctx.destroyBuffer(quadsBuf);
        }

        ctx.destroyBuffer(countsBuf);
        ctx.destroyBuffer(offsetsBuf);
        ctx.destroyBuffer(quadsStub);
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

} // namespace

int main(int argc, char** argv) {
    const uint64_t seed = 20260719; // vxc::SyntheticTileSampler seed, per task spec.
    std::string spvPath = VXC_SPV_PATH;
    std::string voxelizeSpvPath = VXC_SPV_PATH_VOXELIZE;
    std::string meshCountSpvPath = VXC_SPV_PATH_MESHCOUNT;
    std::string meshEmitSpvPath = VXC_SPV_PATH_MESHEMIT;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--spv" && i + 1 < argc) spvPath = argv[++i];
        else if (a == "--spv-voxelize" && i + 1 < argc) voxelizeSpvPath = argv[++i];
        else if (a == "--spv-meshcount" && i + 1 < argc) meshCountSpvPath = argv[++i];
        else if (a == "--spv-meshemit" && i + 1 < argc) meshEmitSpvPath = argv[++i];
    }

    std::printf(
        "voxel-core GPU harness (ADR-0001 M0 gate) — ColumnMain + VoxelizeMain + "
        "MeshCount/EmitMain, seed %llu\n",
        (unsigned long long)seed);

    loadVulkanLoader();
    GpuContext ctx = createContext(spvPath, voxelizeSpvPath, meshCountSpvPath, meshEmitSpvPath);
    std::printf("device: %s\n\n", ctx.deviceName.c_str());

    const RegionSpec regions[] = {
        {"origin", -64, -64, 128, 128},
        {"far-negative", -100000, 250000, 128, 128},
    };

    Digest gpuDigest;
    std::vector<Mismatch> mismatches;
    constexpr size_t kMaxMismatchesPrinted = 20;
    size_t totalColumns = 0;
    size_t totalCells = 0;
    size_t totalMeshedBricks = 0;
    size_t totalQuads = 0;
    double totalColumnDispatchMs = 0;
    double totalVoxelizeDispatchMs = 0;
    double totalMeshCountMs = 0;
    double totalMeshEmitMs = 0;

    for (const RegionSpec& region : regions) {
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

                auto record = [&](const char* field, int64_t cpuVal, int64_t gpuVal) {
                    if (cpuVal != gpuVal && mismatches.size() < kMaxMismatchesPrinted)
                        mismatches.push_back({vx, vy, field, cpuVal, gpuVal});
                };
                record("surfaceMm", c.surfaceMm, g.surfaceMm);
                record("topsoilMm", c.topsoilMm, g.topsoilMm);
                record("subsoilMm", c.subsoilMm, g.subsoilMm);
                record("bedrockDepthMm", c.bedrockDepthMm, g.bedrockDepthMm);
                record("surfaceMat", c.surfaceMat, g.surfaceMat);

                // --- VoxelizeMain cell comparison for this column's brick
                // stack (mirrors GeneratedWorld<8>::makeBrick's per-cell
                // Amplifier::materialAt call; the same column c is reused,
                // never recomputed). Layout matches worldgen.hlsl's
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
            std::printf("[%s] MeshCount dispatch: %.3f ms, MeshEmit dispatch: %.3f ms, "
                        "%u quads over %ux%ux%u interior bricks\n",
                        region.name, gpu.meshCountDispatchMs, gpu.meshEmitDispatchMs,
                        gpu.quadOffsets.empty() ? 0u : gpu.quadOffsets.back(),
                        gpu.interiorBricksX, gpu.interiorBricksY, gpu.interiorBricksZ);
            totalMeshCountMs += gpu.meshCountDispatchMs;
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
    }

    const std::string deviceName = ctx.deviceName;
    destroyContext(ctx);
    FreeLibrary(g_vulkanDll);

    const double voxelizeSec = totalVoxelizeDispatchMs / 1000.0;
    const double cellsPerSec = voxelizeSec > 0.0 ? double(totalCells) / voxelizeSec : 0.0;

    const double meshSec = (totalMeshCountMs + totalMeshEmitMs) / 1000.0;
    const double quadsPerSec = meshSec > 0.0 ? double(totalQuads) / meshSec : 0.0;
    std::printf(
        "\ncompared %zu columns, %zu cells, %zu meshed bricks (%zu quads) across %zu regions\n"
        "  ColumnMain total dispatch wall-clock:   %.3f ms\n"
        "  VoxelizeMain total dispatch wall-clock: %.3f ms  (%.3f Mcells/sec)\n"
        "  Mesh count+emit total wall-clock:       %.3f ms  (%.3f Mquads/sec)\n",
        totalColumns, totalCells, totalMeshedBricks, totalQuads,
        sizeof(regions) / sizeof(regions[0]), totalColumnDispatchMs, totalVoxelizeDispatchMs,
        cellsPerSec / 1e6, totalMeshCountMs + totalMeshEmitMs, quadsPerSec / 1e6);
    std::printf("GPU output digest (columns + cells + quads, combined): %016llx\n",
                (unsigned long long)gpuDigest.h);

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

    std::printf(
        "\nPASS: GPU output is bit-exact with the CPU reference (%s) over %zu columns, %zu "
        "cells, %zu quads\n",
        deviceName.c_str(), totalColumns, totalCells, totalQuads);
    return 0;
}
