include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
# Keep gtest's runtime library selection consistent with the rest of the
# project on Windows instead of forcing its own default.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(zeebulator_tests
  core_test.cpp
  memory_test.cpp
  cpu_test.cpp
  ggz_test.cpp
)
target_include_directories(zeebulator_tests PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(zeebulator_tests PRIVATE zeebulator_core GTest::gtest_main zlibstatic)

include(GoogleTest)
gtest_discover_tests(zeebulator_tests)
