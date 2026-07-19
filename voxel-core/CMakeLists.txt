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
)
target_include_directories(voxelcore PUBLIC include)
if(MSVC)
  target_compile_options(voxelcore PRIVATE /W4 /WX)
else()
  target_compile_options(voxelcore PRIVATE -Wall -Wextra -Wconversion -Werror)
endif()

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)
