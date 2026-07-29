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
  test_counters.cpp
  test_waterca.cpp
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
  test_density3.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(vxc_tests PRIVATE voxelcore Threads::Threads)
target_compile_definitions(vxc_tests PRIVATE
  VXC_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
if(NOT MSVC)
  target_compile_options(vxc_tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME vxc_tests COMMAND vxc_tests)
