cmake_minimum_required(VERSION 3.20)
project(zeebulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ZEEBULATOR_BUILD_LIBRETRO "Build the libretro core" ON)
option(ZEEBULATOR_BUILD_STANDALONE "Build the standalone dev frontend" ON)
option(ZEEBULATOR_BUILD_TESTS "Build the test suite" ON)
option(ZEEBULATOR_BUILD_TOOLS "Build dev tools" ON)

# Declared at top level (not inside core/) so zlib_SOURCE_DIR/zlib_BINARY_DIR
# and the zlibstatic target are visible to sibling subdirectories too
# (e.g. tests/, which also needs zlib directly for building test fixtures).
include(FetchContent)
FetchContent_Declare(
  zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG v1.3.1
)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)

if(ZEEBULATOR_BUILD_STANDALONE)
  FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.9
  )
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL2)

  # System-provided on all three target OSes (opengl32 ships with any
  # Windows toolchain, the OpenGL framework ships with Xcode/macOS, Linux
  # needs its distro's GL dev package -- e.g. libgl1-mesa-dev on
  # Debian/Ubuntu, the same category of build-time-only dependency as the
  # X11 dev headers SDL2 itself needed, see TASKS.md Phase 3).
  find_package(OpenGL REQUIRED)
endif()

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
