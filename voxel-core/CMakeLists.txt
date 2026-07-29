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
  src/channel.cpp
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

# ---------------------------------------------------------------------------
# Opt-in REAL zstd, for the TEST BINARY ONLY. OFF by default, and CI never
# turns it on -- that is not laziness, it is the property under test: the
# CODEC_ZSTD decompressor is injected at the host boundary
# (voxelcore/tilestore.h) precisely so voxel-core builds, links and passes its
# whole suite with no compression library present anywhere. If this option ever
# becomes load-bearing for a green build, the injection design has been broken.
#
# What it buys: without it, every CODEC_ZSTD test runs against this repo's own
# Raw_Block zstd reader (tests/test_tilestore.cpp), which is conformant zstd but
# exercises none of the ENTROPY decoding -- no Huffman literals, no FSE
# sequences. Turning it ON compiles ONE extra test that compresses with a real
# libzstd and decodes the result through the same injected boundary, then
# digest-compares against the CODEC_RAW encoding of the same lattice. That
# equivalence is the only thing that proves a real frame decodes to the right
# NUMBERS; no length or structural check can.
#
# NOTE the deliberate asymmetry: zstd is linked into `vxc_tests` and NEVER into
# `voxelcore`. voxelcore is compiled into a UE binary, and UE 5.8 already
# carries a zstd statically linked inside ThirdParty/Blosc's libblosc.lib (it
# does NOT ship a standalone zstd -- it ships Oodle and LZ4), so a second copy
# vendored here would be an ODR hazard whose failure mode is wrong terrain, not
# a link error. tests/CMakeLists.txt is where the link happens; keep it there.
#
# Resolution order (tests/CMakeLists.txt): VXC_ZSTD_ROOT, then an installed
# zstd, then a pinned source download. Point VXC_ZSTD_URL at a local tarball to
# build this offline.
option(VXC_WITH_ZSTD "Link vxc_tests against a real zstd and run the real-frame \
CODEC_ZSTD test (developer opt-in; never in CI)" OFF)
set(VXC_ZSTD_ROOT "" CACHE PATH
    "Prefix of a prebuilt zstd (expects include/zstd.h and a static/import lib)")
set(VXC_ZSTD_URL
    "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz"
    CACHE STRING "zstd source archive to build when none is installed (may be a local file)")
set(VXC_ZSTD_SHA256 "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3"
    CACHE STRING "SHA256 of VXC_ZSTD_URL; empty to skip the check (local tarballs)")

if(MSVC)
  target_compile_options(voxelcore PRIVATE /W4 /WX)
else()
  target_compile_options(voxelcore PRIVATE -Wall -Wextra -Wconversion -Werror)
endif()

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)
