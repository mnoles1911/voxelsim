add_executable(vxc_bench bench_main.cpp)
target_link_libraries(vxc_bench PRIVATE voxelcore)
if(NOT MSVC)
  target_compile_options(vxc_bench PRIVATE -Wall -Wextra -Werror)
endif()

# --- vxc_waterca_bench: W2 perf-gap step() timing (docs/status.md) --------
add_executable(vxc_waterca_bench waterca_bench.cpp)
target_link_libraries(vxc_waterca_bench PRIVATE voxelcore)
if(NOT MSVC)
  target_compile_options(vxc_waterca_bench PRIVATE -Wall -Wextra -Werror)
endif()

# --- vxc_terrainprobe: coarse-to-fine detail diagnostic. Structure function
# and voxel terrace run lengths of the amplified surface against real tiles;
# the numeric readout behind the ground-level corduroy artifact. ------------
add_executable(vxc_terrainprobe terrainprobe.cpp)
target_link_libraries(vxc_terrainprobe PRIVATE voxelcore)
if(NOT MSVC)
  target_compile_options(vxc_terrainprobe PRIVATE -Wall -Wextra -Werror)
endif()

# --- vxc_stagedump: STAGE-BY-STAGE observability for the amplification
# pipeline. Dumps the client half (S2 carrier-only / S3 continuous surface /
# S4 voxelised) as float32 .npy + JSON sidecars over one world rectangle, at
# both the 30 m coarse tier and the 1.875 m baked fine tier, so a defect can be
# localised to a stage instead of only seen at the end. terrain-service/tools/
# dump_stage_heightfields.py writes S0/S1 in the same shape. ------------------
add_executable(vxc_stagedump stagedump.cpp)
target_link_libraries(vxc_stagedump PRIVATE voxelcore)
if(NOT MSVC)
  target_compile_options(vxc_stagedump PRIVATE -Wall -Wextra -Werror)
endif()

# --- vxc_climateprobe: climate/biome/stratigraphy diagnostic. Channel
# percentiles in u8 AND physical units, biome census WITH gate attribution,
# topsoil depth, and the top-voxel material histogram -- the numeric readout
# behind "the biome material never reaches the player". ---------------------
add_executable(vxc_climateprobe climateprobe.cpp)
target_link_libraries(vxc_climateprobe PRIVATE voxelcore)
if(NOT MSVC)
  target_compile_options(vxc_climateprobe PRIVATE -Wall -Wextra -Werror)
endif()

# --- vxc_riverprobe: W3 channel-carving diagnostic. Network census, the
# headwater-to-mouth long profile, centreline continuity, and the bank
# containment check -- the numeric readout behind "there is a riverbed for
# water to flow in". -------------------------------------------------------
add_executable(vxc_riverprobe riverprobe.cpp)
target_link_libraries(vxc_riverprobe PRIVATE voxelcore)
if(NOT MSVC)
  target_compile_options(vxc_riverprobe PRIVATE -Wall -Wextra -Werror)
endif()

# --- vxc_dump_biome_constants: prints biome.h's compile-time-derived
# thresholds as plain integers, for hand-transcription into worldgen.ush's
# mirror (HLSL has no climate.h). Hand-arithmetic is how a mirror drifts. -----
add_executable(vxc_dump_biome_constants dump_biome_constants.cpp)
target_link_libraries(vxc_dump_biome_constants PRIVATE voxelcore)
if(NOT MSVC)
  target_compile_options(vxc_dump_biome_constants PRIVATE -Wall -Wextra -Werror)
endif()

# --- vxc_editlog: offline edit-log stats/compact/verify tool (M3 groundwork,
# docs/status.md) -----------------------------------------------------------
add_executable(vxc_editlog editlog_tool.cpp)
target_link_libraries(vxc_editlog PRIVATE voxelcore)
if(NOT MSVC)
  target_compile_options(vxc_editlog PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME vxc_editlog_selftest COMMAND vxc_editlog selftest)

# --- vxc_gpu: Vulkan worldgen determinism harness (ADR-0001) ---------------
# Headless: dispatches voxel-core/shaders/prebuilt/worldgen.ColumnMain.spv
# (etc.) and byte-compares against the CPU reference. Builds on Windows and
# Linux (M0 cross-vendor NVIDIA-vs-AMD gate close-out); the .spv kernels are
# committed under voxel-core/shaders/prebuilt/ (compiled once from
# voxel-core/shaders/worldgen.ush via the Windows-only DXC flow,
# tools/compile-shaders.ps1 — SPIR-V is portable bytecode, so the SAME
# committed bytes run unmodified on both platforms; see prebuilt/README.md
# for provenance/hashes). No Vulkan SDK dependency either way: the loader
# (vulkan-1.dll / libvulkan.so.1) is resolved at runtime
# (LoadLibrary+GetProcAddress on Windows, dlopen+dlsym on Linux), so only
# headers are required to build — tools/vulkan-headers
# (tools/fetch-vulkan-headers.ps1) on Windows, the system Vulkan headers
# (e.g. `apt-get install libvulkan-dev`) via find_package(Vulkan) on Linux.
get_filename_component(VXC_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VXC_VULKAN_HEADERS_DIR "${VXC_REPO_ROOT}/tools/vulkan-headers/include")
set(VXC_SPV_DIR "${VXC_REPO_ROOT}/voxel-core/shaders/prebuilt")

set(_vxc_gpu_default OFF)
# Auto-default is MSVC-only (not "any WIN32 compiler"): gpu_harness.cpp's
# Vulkan struct aggregate-initializers (e.g. VkBufferCreateInfo bci{...})
# leave trailing fields to value-initialization, which MSVC accepts warning-
# free but clang's -Wmissing-field-initializers (on, under this target's
# -Wall -Wextra -Werror) rejects — this file has only ever been built with
# MSVC in this repo's history. Gating on MSVC specifically means a Windows
# box that happens to have both tools/vulkan-headers fetched AND a
# clang/llvm-mingw toolchain configured as CMAKE_CXX_COMPILER (a real local
# dev setup, not hypothetical) still gets a clean default configure/build —
# same as before tools/vulkan-headers existed. Opt in explicitly
# (-DVXC_BUILD_GPU_HARNESS=ON) to try clang anyway.
if(MSVC AND EXISTS "${VXC_VULKAN_HEADERS_DIR}/vulkan/vulkan_core.h")
  set(_vxc_gpu_default ON)
endif()
# VXC_BUILD_GPU_HARNESS is opt-in on Linux (no default-ON autodetection via
# find_package(Vulkan) here) so a plain default configure — the one every
# other CI job (gcc/clang/msvc) relies on — never gains a new hard
# dependency on system Vulkan headers just by existing on Linux.
option(VXC_BUILD_GPU_HARNESS
  "Build vxc_gpu, the Vulkan worldgen determinism harness (ADR-0001)"
  ${_vxc_gpu_default})

if(VXC_BUILD_GPU_HARNESS)
  if(NOT EXISTS "${VXC_SPV_DIR}/worldgen.ColumnMain.spv")
    message(FATAL_ERROR
      "VXC_BUILD_GPU_HARNESS is ON but ${VXC_SPV_DIR}/worldgen.ColumnMain.spv is missing - "
      "the committed SPIR-V kernels (voxel-core/shaders/prebuilt/) should always be present; "
      "see prebuilt/README.md")
  endif()

  add_executable(vxc_gpu gpu_harness.cpp)
  target_link_libraries(vxc_gpu PRIVATE voxelcore)

  if(WIN32)
    if(NOT EXISTS "${VXC_VULKAN_HEADERS_DIR}/vulkan/vulkan_core.h")
      message(FATAL_ERROR
        "VXC_BUILD_GPU_HARNESS is ON but ${VXC_VULKAN_HEADERS_DIR}/vulkan/vulkan_core.h is "
        "missing - run tools/fetch-vulkan-headers.ps1 first")
    endif()
    target_include_directories(vxc_gpu PRIVATE "${VXC_VULKAN_HEADERS_DIR}")
    # vulkan-1.dll is loaded dynamically (LoadLibrary + vkGetInstanceProcAddr);
    # no vulkan-1.lib import library needed or linked.
  else()
    # Linux: use the system Vulkan headers (apt-get install libvulkan-dev
    # provides <vulkan/vulkan_core.h>), not tools/vulkan-headers (a
    # Windows-flow-only fetch). No import lib needed here either — libvulkan
    # is opened via dlopen+dlsym at runtime, so only headers matter, but
    # dlopen/dlsym themselves need libdl.
    find_package(Vulkan REQUIRED)
    target_include_directories(vxc_gpu PRIVATE "${Vulkan_INCLUDE_DIRS}")
    target_link_libraries(vxc_gpu PRIVATE ${CMAKE_DL_LIBS})
  endif()

  # SPIR-V kernels: committed under voxel-core/shaders/prebuilt/, identical
  # path on both platforms (see the file-level comment above).
  target_compile_definitions(vxc_gpu PRIVATE
    VXC_SPV_PATH="${VXC_SPV_DIR}/worldgen.ColumnMain.spv"
    VXC_SPV_PATH_VOXELIZE="${VXC_SPV_DIR}/worldgen.VoxelizeMain.spv"
    VXC_SPV_PATH_MESHCOUNT="${VXC_SPV_DIR}/worldgen.MeshCountMain.spv"
    VXC_SPV_PATH_MESHEMIT="${VXC_SPV_DIR}/worldgen.MeshEmitMain.spv"
    VXC_SPV_PATH_SCANBLOCKS="${VXC_SPV_DIR}/worldgen.ScanBlocksMain.spv"
    VXC_SPV_PATH_SCANSUMS="${VXC_SPV_DIR}/worldgen.ScanSumsMain.spv"
    VXC_SPV_PATH_SCANADD="${VXC_SPV_DIR}/worldgen.ScanAddMain.spv"
  )
  if(NOT MSVC)
    # gpu_harness.cpp uses the idiomatic Vulkan aggregate-init pattern
    # (e.g. VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_...}) that deliberately
    # value-initializes (zeroes) the trailing members. GCC/clang -Wextra flags
    # that as -Wmissing-field-initializers; under -Werror it's fatal. The
    # zero-fill is intentional and correct, so exclude just that one warning
    # (real issues still error). Without this, the opt-in Linux/GCC build path
    # — exactly what tools/run-nvidia-digest.sh drives — fails to compile.
    target_compile_options(vxc_gpu PRIVATE
      -Wall -Wextra -Werror -Wno-missing-field-initializers)
  endif()
endif()
