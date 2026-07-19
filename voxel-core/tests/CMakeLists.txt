add_executable(vxc_tests
  test_main.cpp
  test_brick.cpp
  test_hash.cpp
  test_amplifier.cpp
  test_mesher.cpp
  test_editlog.cpp
  test_world.cpp
  test_mips.cpp
)
target_link_libraries(vxc_tests PRIVATE voxelcore)
if(NOT MSVC)
  target_compile_options(vxc_tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME vxc_tests COMMAND vxc_tests)
