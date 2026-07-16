add_library(zeebulator_core STATIC
  core.cpp
  core.h
  backend.h
)

target_include_directories(zeebulator_core PUBLIC ${PROJECT_SOURCE_DIR})
