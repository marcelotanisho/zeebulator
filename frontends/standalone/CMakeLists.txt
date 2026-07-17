add_executable(zeebulator_standalone main.cpp sdl2_gl_backend.cpp sdl2_gl_backend.h)
target_link_libraries(zeebulator_standalone PRIVATE zeebulator_core SDL2::SDL2 SDL2::SDL2main
                                                     OpenGL::GL)
target_compile_definitions(zeebulator_standalone PRIVATE
  HELLO_BREW_FIXTURE_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/hello_brew/hello_brew.bin")
