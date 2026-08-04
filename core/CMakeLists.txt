configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/audio/soundfont_path.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/soundfont_path.h
  @ONLY
)

add_library(zeebulator_core STATIC
  core.cpp
  core.h
  backend.h
  save_state.cpp
  save_state.h
  memory/memory.cpp
  memory/memory.h
  cpu/arm_core.h
  cpu/arm_interpreter.cpp
  cpu/arm_interpreter.h
  loader/bar.cpp
  loader/bar.h
  loader/ggz.cpp
  loader/ggz.h
  loader/mif.cpp
  loader/mif.h
  loader/mod.cpp
  loader/mod.h
  loader/obm1.cpp
  loader/obm1.h
  loader/pkg.cpp
  loader/pkg.h
  loader/wav.cpp
  loader/wav.h
  loader/midi.cpp
  loader/midi.h
  loader/atitc.cpp
  loader/atitc.h
  audio/mixer.cpp
  audio/mixer.h
  audio/soundfont_synth.cpp
  audio/soundfont_synth.h
  brew/hle_runtime.cpp
  brew/hle_runtime.h
  brew/interface_object.cpp
  brew/interface_object.h
  brew/scaffold_object.cpp
  brew/scaffold_object.h
  brew/ishell.cpp
  brew/ishell.h
  brew/idisplay.cpp
  brew/idisplay.h
  brew/font5x7.cpp
  brew/font5x7.h
  brew/virtual_filesystem.cpp
  brew/virtual_filesystem.h
  brew/file_hle.cpp
  brew/file_hle.h
  brew/media_hle.cpp
  brew/media_hle.h
  brew/mod_runtime.cpp
  brew/mod_runtime.h
  brew/gl_types.h
  brew/gl_backend.h
  brew/gl_hle.cpp
  brew/gl_hle.h
  brew/hid_hle.cpp
  brew/hid_hle.h
)

target_include_directories(zeebulator_core PUBLIC ${PROJECT_SOURCE_DIR})
# zlib's CMakeLists predates modern usage-requirement propagation, so its
# generated zconf.h (in the binary dir) needs to be added explicitly.
target_include_directories(zeebulator_core PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(zeebulator_core PRIVATE zlibstatic)
# TinySoundFont is header-only (no CMakeLists of its own to depend on);
# the generated/ dir holds this build's own configure_file output
# (soundfont_path.h, the real fetched .sf2's real absolute path).
target_include_directories(zeebulator_core PRIVATE
  ${tinysoundfont_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
