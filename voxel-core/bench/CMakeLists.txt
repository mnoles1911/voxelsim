add_executable(vxc_bench bench_main.cpp)
target_link_libraries(vxc_bench PRIVATE voxelcore)
if(NOT MSVC)
  target_compile_options(vxc_bench PRIVATE -Wall -Wextra -Werror)
endif()

# --- vxc_gpu: Vulkan worldgen determinism harness (ADR-0001) ---------------
# Headless: dispatches build/shaders/worldgen.ColumnMain.spv and byte-compares
# against the CPU reference. Windows-only for now; needs tools/vulkan-headers
# (tools/fetch-vulkan-headers.ps1) at configure time — no Vulkan SDK, the
# loader (vulkan-1.dll) is resolved at runtime, so only headers are required
# to build.
get_filename_component(VXC_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VXC_VULKAN_HEADERS_DIR "${VXC_REPO_ROOT}/tools/vulkan-headers/include")

set(_vxc_gpu_default OFF)
if(WIN32 AND EXISTS "${VXC_VULKAN_HEADERS_DIR}/vulkan/vulkan_core.h")
  set(_vxc_gpu_default ON)
endif()
option(VXC_BUILD_GPU_HARNESS
  "Build vxc_gpu, the Vulkan worldgen determinism harness (ADR-0001, Windows-only)"
  ${_vxc_gpu_default})

if(VXC_BUILD_GPU_HARNESS)
  if(NOT WIN32)
    message(FATAL_ERROR "VXC_BUILD_GPU_HARNESS is Windows-only for now (ADR-0001 M0 scope)")
  endif()
  if(NOT EXISTS "${VXC_VULKAN_HEADERS_DIR}/vulkan/vulkan_core.h")
    message(FATAL_ERROR
      "VXC_BUILD_GPU_HARNESS is ON but ${VXC_VULKAN_HEADERS_DIR}/vulkan/vulkan_core.h is "
      "missing - run tools/fetch-vulkan-headers.ps1 first")
  endif()

  add_executable(vxc_gpu gpu_harness.cpp)
  target_link_libraries(vxc_gpu PRIVATE voxelcore)
  target_include_directories(vxc_gpu PRIVATE "${VXC_VULKAN_HEADERS_DIR}")
  # vulkan-1.dll is loaded dynamically (LoadLibrary + vkGetInstanceProcAddr);
  # no vulkan-1.lib import library needed or linked.
  target_compile_definitions(vxc_gpu PRIVATE
    VXC_SPV_PATH="${VXC_REPO_ROOT}/build/shaders/worldgen.ColumnMain.spv"
  )
  if(NOT MSVC)
    target_compile_options(vxc_gpu PRIVATE -Wall -Wextra -Werror)
  endif()
endif()
