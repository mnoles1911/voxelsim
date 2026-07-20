add_executable(vxc_tests
  test_main.cpp
  test_brick.cpp
  test_hash.cpp
  test_amplifier.cpp
  test_mesher.cpp
  test_editlog.cpp
  test_editcompact.cpp
  test_world.cpp
  test_mips.cpp
  test_raycast.cpp
  test_tilestore.cpp
  test_counters.cpp
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
