cmake_minimum_required(VERSION 3.20)
project(voxel-core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(voxelcore STATIC
  src/amplifier.cpp
  src/tilestore.cpp
  src/tilestreaming.cpp
  src/waterca.cpp
  src/rivernet.cpp
  src/rivercouple.cpp
  src/swe.cpp
)
target_include_directories(voxelcore PUBLIC include)

# Opt-in memo instrumentation. OFF by default: it adds a thread_local increment
# to the hottest read in worldgen, and the point of the counters is to measure
# the code, not the instrument. PUBLIC so consumers (vxc_bench) see the same
# declarations the library was built with -- getting that wrong gives an ODR
# mismatch rather than a compile error.
option(VXC_MEMO_STATS "Count amplifier tile-memo probes/misses (diagnostic)" OFF)
if(VXC_MEMO_STATS)
  target_compile_definitions(voxelcore PUBLIC VXC_MEMO_STATS)
endif()
# Opt-in per-phase WaterCA::step() accounting (waterca.h WaterCAProfile). OFF
# by default for the same reason VXC_MEMO_STATS is: the point of the counters
# is to measure the code, not the instrument, and every timing quoted in
# docs/status.md must be taken on an uninstrumented build. PUBLIC so
# vxc_waterca_bench sees the same declarations the library was built with.
option(VXC_WATER_PROFILE "Accumulate per-phase WaterCA::step() timings (diagnostic)" OFF)
if(VXC_WATER_PROFILE)
  target_compile_definitions(voxelcore PUBLIC VXC_WATER_PROFILE)
endif()

if(MSVC)
  target_compile_options(voxelcore PRIVATE /W4 /WX)
else()
  target_compile_options(voxelcore PRIVATE -Wall -Wextra -Wconversion -Werror)
endif()

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)
