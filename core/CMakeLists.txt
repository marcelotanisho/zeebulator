add_library(zeebulator_core STATIC
  core.cpp
  core.h
  backend.h
  memory/memory.cpp
  memory/memory.h
  cpu/arm_core.h
  cpu/arm_interpreter.cpp
  cpu/arm_interpreter.h
)

target_include_directories(zeebulator_core PUBLIC ${PROJECT_SOURCE_DIR})
