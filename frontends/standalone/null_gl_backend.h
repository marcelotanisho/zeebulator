#pragma once

#include <cstdint>

#include "core/brew/gl_backend.h"

namespace zeebulator {

// A GlBackend that never touches any real host graphics API: every
// method either does nothing or returns a plausible, harmless success
// value, so real app code that calls into IGL/IEGL still gets sane
// results (CreateContext succeeding, GenTextures handing back distinct
// nonzero names, ...) without a single real SDL_GL_CreateContext,
// glClear, or glDrawArrays ever executing.
//
// Deliberately used instead of Sdl2GlBackend by tools/game_probe.cpp
// (and anywhere else a real 2D IDisplay surface is the frontend's actual
// visible output): confirmed via real Double Dragon disassembly and a
// minimal, independent SDL2 reproduction (TASKS.md Phase 8) that merely
// having a *second*, real host GL context anywhere in the process --
// even one on its own private, hidden window, and even affecting a
// completely separate window that never itself touches GL -- reliably
// and permanently breaks this desktop's real window compositor
// (Cinnamon/Muffin) into no longer repainting that other, real,
// correctly-updating window at all, until something forces the
// compositor to recompute its geometry (e.g. an interactive title-bar
// drag) -- a real environment bug, not anything in this project's own
// present logic, and not one any tried application-level workaround
// (a private hidden GL window, disabling SDL's _NET_WM_BYPASS_COMPOSITOR
// hint, forcing continuous synthetic window-position changes) was able
// to work around. Real GLES rendering also isn't a complete, correct
// pipeline yet for actual games regardless (this same Double Dragon
// trace showed a degenerate glViewport(0,0,1,0) call, an unrelated real
// gap of its own) -- so, until either the compositor bug or the
// rendering gap is actually resolved, real GL context creation must not
// be allowed to clobber the surface real games' actual visible output
// (their 2D IDisplay content) depends on. frontends/standalone/main.cpp's
// isolated hello_gl demo has no competing 2D surface in its own process
// and keeps using the real Sdl2GlBackend unaffected.
class NullGlBackend : public GlBackend {
 public:
  bool CreateContext() override { return true; }
  void DestroyContext() override {}
  void SwapBuffers() override {}

  void Clear(GLbitfield) override {}
  void ClearColor(float, float, float, float) override {}
  void Viewport(int, int, int, int) override {}
  void Enable(GLenum) override {}
  void Disable(GLenum) override {}
  void MatrixMode(GLenum) override {}
  void LoadIdentity() override {}
  void Ortho(float, float, float, float, float, float) override {}
  void Frustum(float, float, float, float, float, float) override {}
  void Translate(float, float, float) override {}
  void Rotate(float, float, float, float) override {}
  void Scale(float, float, float) override {}
  void Color4(float, float, float, float) override {}
  void DrawArrays(GLenum, const GlVertexArrays&) override {}

  void GenTextures(GLsizei n, GLuint* textures) override {
    for (GLsizei i = 0; i < n; ++i) textures[i] = next_texture_name_++;
  }
  void DeleteTextures(GLsizei, const GLuint*) override {}
  void BindTexture(GLenum, GLuint) override {}
  void TexParameter(GLenum, GLenum, GLint) override {}
  void TexImage2D(GLenum, const GlTextureImage&) override {}

 private:
  GLuint next_texture_name_ = 1;  // 0 is reserved (means "no texture") in real GL
};

}  // namespace zeebulator
