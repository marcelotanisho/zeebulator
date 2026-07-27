#pragma once

#include <SDL.h>

#include "core/backend.h"
#include "core/brew/gl_backend.h"

namespace zeebulator {

// Implements both Backend (2D IDisplay video/audio/input) and GlBackend
// (real IGL/IEGL rendering) on top of a single real SDL_Window + single
// real SDL_GLContext, created eagerly at construction and held for the
// object's entire lifetime.
//
// Exists specifically to avoid a real, confirmed desktop compositor bug
// (TASKS.md/PHASE8_LOG.md Phase 8, Double Dragon): a real host GL
// context anywhere in this process, coexisting with a *separate* 2D
// SDL_Renderer-backed presentation path (even a different, hidden
// window's own context), reliably breaks this desktop's real compositor
// (Cinnamon/Muffin) into never repainting the real visible window again.
// A minimal, independent reproduction (same investigation) confirmed a
// *single* real GL context used as the sole presentation mechanism for
// the one real visible window does NOT trigger this bug, no matter how
// much real GL activity (repeated clear/draw/swap) it does. So instead
// of a separate SDL_Renderer + a second/hidden GL context
// (Sdl2Backend + Sdl2GlBackend, still used where only one of the two
// presentation needs applies -- see their own doc comments), this class
// presents the 2D IDisplay framebuffer *through* the same real GL
// context real GLES rendering uses: PushVideoFrame uploads it as a
// texture and draws a full-screen textured quad, then swaps -- the
// exact same real drawable, the exact same real context, every time.
//
// This also matches what real Double Dragon disassembly found
// elsewhere in this investigation: `IBITMAP_QueryInterface(...,
// AEECLSID_DIB, ...)`'s result gets cast straight to `NativeWindowType`
// for `eglCreateWindowSurface` -- i.e. on real hardware, GLES's own
// native window surface *is* IDisplay's own device bitmap/surface, not
// a separate one. Real code drawing 2D IDisplay content and real code
// drawing GLES content are, on real hardware, both just drawing to the
// one real display surface -- exactly what this class does here.
class Sdl2UnifiedBackend : public Backend, public GlBackend {
 public:
  Sdl2UnifiedBackend(SDL_Window* window, int width, int height, int audio_sample_rate);
  ~Sdl2UnifiedBackend() override;

  // Backend
  void PushVideoFrame(const void* framebuffer, int width, int height,
                       PixelFormat format) override;
  void PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                         int sample_rate) override;
  ZPadState PollInput() override;

  // True once the real app has called eglSwapBuffers (SwapBuffers()
  // below) at least once. Real Double Dragon disassembly (TASKS.md
  // Phase 8) confirmed the app stops calling IDISPLAY_Update entirely
  // once it starts real GL rendering -- so the frontend's own
  // "RepresentLastFrame" keep-alive mechanism (idisplay.h) has nothing
  // useful left to re-present from that point on, and doing so anyway
  // actively fights the app's own real GL swaps for the same drawable
  // (confirmed on the real desktop: real GL content never became
  // visible until the frontend stopped re-pushing the stale 2D
  // snapshot once this goes true).
  bool HasRealGlActivity() const { return gl_swap_seen_; }

  // GlBackend -- CreateContext/DestroyContext are no-ops (true/nothing):
  // this class's one real context is created in the constructor and
  // lives until the destructor, regardless of the app's own real
  // eglMakeCurrent/eglDestroyContext/eglTerminate call pattern, since
  // the 2D video path depends on it too.
  bool CreateContext() override;
  void DestroyContext() override;
  void SwapBuffers() override;

  void Clear(GLbitfield mask) override;
  void ClearColor(float r, float g, float b, float a) override;
  void Viewport(int x, int y, int width, int height) override;
  void Enable(GLenum cap) override;
  void Disable(GLenum cap) override;
  void MatrixMode(GLenum mode) override;
  void LoadIdentity() override;
  void Ortho(float left, float right, float bottom, float top, float near_plane,
             float far_plane) override;
  void Frustum(float left, float right, float bottom, float top, float near_plane,
               float far_plane) override;
  void Translate(float x, float y, float z) override;
  void Rotate(float angle_degrees, float x, float y, float z) override;
  void Scale(float x, float y, float z) override;
  void Color4(float r, float g, float b, float a) override;
  void AlphaFunc(GLenum func, float ref) override;
  void BlendFunc(GLenum sfactor, GLenum dfactor) override;
  void DrawArrays(GLenum mode, const GlVertexArrays& arrays) override;

  void GenTextures(GLsizei n, GLuint* textures) override;
  void DeleteTextures(GLsizei n, const GLuint* textures) override;
  void BindTexture(GLenum target, GLuint texture) override;
  void TexParameter(GLenum target, GLenum pname, GLint param) override;
  void TexImage2D(GLenum target, const GlTextureImage& image) override;

 private:
  ZPadState PollController();
  ZPadState PollKeyboard();

  // Common tail of both real presentation paths (PushVideoFrame's 2D
  // quad and the real app's own eglSwapBuffers): updates the rolling
  // FPS estimate, draws it as a small top-left overlay (immediate-mode
  // GL quads, matching the save/restore-state pattern PushVideoFrame
  // already uses, so it never corrupts real app-owned GL state), then
  // does the one real SDL_GL_SwapWindow for this frame.
  void PresentFrame();
  void DrawFpsOverlay();

  SDL_Window* window_;
  SDL_GLContext gl_context_;
  int width_;
  int height_;
  GLuint video_texture_ = 0;  // lazily created on first PushVideoFrame
  bool gl_swap_seen_ = false;

  Uint32 fps_last_tick_ms_ = 0;
  int fps_frame_count_ = 0;
  double fps_display_value_ = 0.0;

  SDL_AudioDeviceID audio_device_ = 0;
  int audio_sample_rate_;
  SDL_GameController* controller_ = nullptr;
};

}  // namespace zeebulator
