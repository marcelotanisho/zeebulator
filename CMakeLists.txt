cmake_minimum_required(VERSION 3.20)
project(zeebulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ZEEBULATOR_BUILD_LIBRETRO "Build the libretro core" ON)
option(ZEEBULATOR_BUILD_STANDALONE "Build the standalone dev frontend" ON)

add_subdirectory(core)

if(ZEEBULATOR_BUILD_STANDALONE)
  add_subdirectory(frontends/standalone)
endif()

if(ZEEBULATOR_BUILD_LIBRETRO)
  add_subdirectory(frontends/libretro)
endif()
