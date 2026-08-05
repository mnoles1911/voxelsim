add_executable(vxc_tests
  test_main.cpp
  test_brick.cpp
  test_hash.cpp
  test_amplifier.cpp
  test_carrier.cpp
  test_biome.cpp
  test_climate.cpp
  test_mesher.cpp
  test_editlog.cpp
  test_editcompact.cpp
  test_world.cpp
  test_mips.cpp
  test_raycast.cpp
  test_tilestore.cpp
  test_tilestreaming.cpp
  test_tilerange.cpp
  test_counters.cpp
  test_waterca.cpp
  test_waterwindow.cpp
  test_connectivity.cpp
  test_collapse.cpp
  test_rivernet.cpp
  test_rivercouple.cpp
  test_pathfind.cpp
  test_regiongraph.cpp
  test_caves.cpp
  test_caverns.cpp
  test_coarsegen.cpp
  test_swe.cpp
  test_detail_rill.cpp
  test_detail_bedding.cpp
  test_channel.cpp
  test_sealevel.cpp
  test_lakes.cpp
  test_riverribbon.cpp
  test_ocean.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(vxc_tests PRIVATE voxelcore Threads::Threads)
target_compile_definitions(vxc_tests PRIVATE
  VXC_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
if(NOT MSVC)
  target_compile_options(vxc_tests PRIVATE -Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------
# VXC_WITH_ZSTD (declared in ../CMakeLists.txt): link a REAL zstd into the TEST
# BINARY, and only the test binary.
#
# `voxelcore` is untouched by everything below and must stay that way -- the
# whole point of the injected FineDecompressor is that the library has no
# compression dependency to collide with the host's. What gets linked here is
# the *host* in the test's world: vxc_tests plays the role the UE module plays
# in the shipped one, and hands voxel-core a callback over a zstd it chose.
#
# Three ways to find one, cheapest first, so a box that already has zstd never
# downloads anything:
#   1. -DVXC_ZSTD_ROOT=<prefix>   (a prebuilt zstd, e.g. vcpkg's)
#   2. an installed zstd on the default search paths (CONFIG package, then
#      plain header+library probing)
#   3. a pinned source archive built here as a static lib. Set VXC_ZSTD_URL to
#      a local file to do this with no network.
# ---------------------------------------------------------------------------
if(VXC_WITH_ZSTD)
  set(_vxc_zstd_target "")

  if(VXC_ZSTD_ROOT)
    find_path(VXC_ZSTD_INCLUDE_DIR zstd.h HINTS "${VXC_ZSTD_ROOT}"
              PATH_SUFFIXES include NO_DEFAULT_PATH)
    find_library(VXC_ZSTD_LIBRARY NAMES zstd libzstd zstd_static libzstd_static
                 HINTS "${VXC_ZSTD_ROOT}" PATH_SUFFIXES lib lib64 NO_DEFAULT_PATH)
    if(NOT VXC_ZSTD_INCLUDE_DIR OR NOT VXC_ZSTD_LIBRARY)
      message(FATAL_ERROR "VXC_ZSTD_ROOT=${VXC_ZSTD_ROOT} has no zstd.h + zstd library")
    endif()
  else()
    find_package(zstd CONFIG QUIET)
    if(TARGET zstd::libzstd_static)
      set(_vxc_zstd_target zstd::libzstd_static)
    elseif(TARGET zstd::libzstd_shared)
      set(_vxc_zstd_target zstd::libzstd_shared)
    elseif(TARGET zstd::libzstd)
      set(_vxc_zstd_target zstd::libzstd)
    else()
      find_path(VXC_ZSTD_INCLUDE_DIR zstd.h)
      find_library(VXC_ZSTD_LIBRARY NAMES zstd libzstd zstd_static libzstd_static)
    endif()
  endif()

  if(NOT _vxc_zstd_target AND NOT (VXC_ZSTD_INCLUDE_DIR AND VXC_ZSTD_LIBRARY))
    # Last resort: build zstd from a pinned archive. Its CMake project lives in
    # build/cmake, not at the archive root, hence SOURCE_SUBDIR. Only the static
    # library is built -- no programs, no tests, no legacy format support, none
    # of which this test has any use for.
    include(FetchContent)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    if(VXC_ZSTD_SHA256)
      FetchContent_Declare(zstd URL "${VXC_ZSTD_URL}" URL_HASH "SHA256=${VXC_ZSTD_SHA256}"
                           SOURCE_SUBDIR build/cmake)
    else()
      FetchContent_Declare(zstd URL "${VXC_ZSTD_URL}" SOURCE_SUBDIR build/cmake)
    endif()
    FetchContent_MakeAvailable(zstd)
    if(TARGET libzstd_static)
      set(_vxc_zstd_target libzstd_static)
    elseif(TARGET zstd::libzstd_static)
      set(_vxc_zstd_target zstd::libzstd_static)
    else()
      message(FATAL_ERROR "built zstd from ${VXC_ZSTD_URL} but found no static target")
    endif()
    # zstd's own CMake exports its include dir on the shared target only in some
    # releases; add it unconditionally so this does not depend on that.
    target_include_directories(vxc_tests PRIVATE "${zstd_SOURCE_DIR}/lib")
  endif()

  if(_vxc_zstd_target)
    target_link_libraries(vxc_tests PRIVATE ${_vxc_zstd_target})
    message(STATUS "vxc_tests: real zstd via target ${_vxc_zstd_target}")
  else()
    target_include_directories(vxc_tests PRIVATE "${VXC_ZSTD_INCLUDE_DIR}")
    target_link_libraries(vxc_tests PRIVATE "${VXC_ZSTD_LIBRARY}")
    message(STATUS "vxc_tests: real zstd via ${VXC_ZSTD_LIBRARY}")
  endif()
  target_compile_definitions(vxc_tests PRIVATE VXC_WITH_ZSTD)
endif()

add_test(NAME vxc_tests COMMAND vxc_tests)
