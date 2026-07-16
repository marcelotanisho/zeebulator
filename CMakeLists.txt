cmake_minimum_required(VERSION 3.20)
project(zeebulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ZEEBULATOR_BUILD_LIBRETRO "Build the libretro core" ON)
option(ZEEBULATOR_BUILD_STANDALONE "Build the standalone dev frontend" ON)
option(ZEEBULATOR_BUILD_TESTS "Build the test suite" ON)
option(ZEEBULATOR_BUILD_TOOLS "Build dev tools" ON)

add_subdirectory(core)

if(ZEEBULATOR_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(ZEEBULATOR_BUILD_STANDALONE)
  add_subdirectory(frontends/standalone)
endif()

if(ZEEBULATOR_BUILD_LIBRETRO)
  add_subdirectory(frontends/libretro)
endif()

if(ZEEBULATOR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
