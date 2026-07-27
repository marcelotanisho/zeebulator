#include "frontends/standalone/sdl2_unified_backend.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

namespace zeebulator {

namespace {

// Minimal 3x5 dot-matrix font, just the glyphs the FPS overlay needs.
// Each row's 3 bits are columns left..right (bit2=leftmost).
struct FontGlyph {
  char c;
  uint8_t rows[5];
};

constexpr FontGlyph kFont3x5[] = {
    {'0', {0x7, 0x5, 0x5, 0x5, 0x7}}, {'1', {0x2, 0x6, 0x2, 0x2, 0x7}},
    {'2', {0x7, 0x1, 0x7, 0x4, 0x7}}, {'3', {0x7, 0x1, 0x7, 0x1, 0x7}},
    {'4', {0x5, 0x5, 0x7, 0x1, 0x1}}, {'5', {0x7, 0x4, 0x7, 0x1, 0x7}},
    {'6', {0x7, 0x4, 0x7, 0x5, 0x7}}, {'7', {0x7, 0x1, 0x1, 0x1, 0x1}},
    {'8', {0x7, 0x5, 0x7, 0x5, 0x7}}, {'9', {0x7, 0x5, 0x7, 0x1, 0x7}},
    {'F', {0x7, 0x4, 0x7, 0x4, 0x4}}, {'P', {0x7, 0x5, 0x7, 0x4, 0x4}},
    {'S', {0x7, 0x4, 0x7, 0x1, 0x7}}, {':', {0x0, 0x2, 0x0, 0x2, 0x0}},
    {' ', {0x0, 0x0, 0x0, 0x0, 0x0}},
};

const FontGlyph* FindGlyph(char c) {
  for (const auto& glyph : kFont3x5) {
    if (glyph.c == c) return &glyph;
  }
  return nullptr;
}

}  // namespace

Sdl2UnifiedBackend::Sdl2UnifiedBackend(SDL_Window* window, int width, int height,
                                        int audio_sample_rate)
    : window_(window),
      gl_context_(SDL_GL_CreateContext(window)),
      width_(width),
      height_(height),
      audio_sample_rate_(audio_sample_rate) {
  if (gl_context_ == nullptr) {
    std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
  } else {
    SDL_GL_MakeCurrent(window_, gl_context_);
    // Explicit real vsync: tried both 0 and 1 directly against the real
    // desktop compositor bug this class exists to avoid (TASKS.md Phase
    // 8) -- 1 measurably reduced (did not fully eliminate) a separate,
    // much smaller residual real compositor quirk (brief, periodic
    // black flashes, unrelated to and far less severe than the
    // permanent blackout the single-context design itself fixes) versus
    // 0 or leaving it at whatever the driver defaults to.
    if (SDL_GL_SetSwapInterval(1) != 0) {
      std::fprintf(stderr, "SDL_GL_SetSwapInterval(1) failed: %s\n", SDL_GetError());
    }
  }

  SDL_AudioSpec desired{};
  desired.freq = audio_sample_rate;
  desired.format = AUDIO_S16SYS;
  desired.channels = 2;
  desired.samples = 1024;

  SDL_AudioSpec obtained{};
  audio_device_ = SDL_OpenAudioDevice(nullptr, /*iscapture=*/0, &desired, &obtained,
                                       /*allowed_changes=*/0);
  if (audio_device_ == 0) {
    std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
  } else {
    SDL_PauseAudioDevice(audio_device_, 0);  // start the device unpaused
  }

  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i)) {
      controller_ = SDL_GameControllerOpen(i);
      if (controller_ != nullptr) break;
    }
  }
}

Sdl2UnifiedBackend::~Sdl2UnifiedBackend() {
  if (controller_ != nullptr) SDL_GameControllerClose(controller_);
  if (audio_device_ != 0) SDL_CloseAudioDevice(audio_device_);
  if (gl_context_ != nullptr) {
    if (video_texture_ != 0) glDeleteTextures(1, &video_texture_);
    SDL_GL_DeleteContext(gl_context_);
  }
}

void Sdl2UnifiedBackend::PushVideoFrame(const void* framebuffer, int width, int height,
                                         PixelFormat format) {
  (void)format;  // IDisplayHle's framebuffer is always RGB565 for now.
  if (gl_context_ == nullptr) return;
  SDL_GL_MakeCurrent(window_, gl_context_);

  bool first_time = video_texture_ == 0;
  if (first_time) {
    glGenTextures(1, &video_texture_);
  }
  glBindTexture(GL_TEXTURE_2D, video_texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  // RGB565 maps directly onto GL's own packed GL_UNSIGNED_SHORT_5_6_5
  // format -- no pixel conversion needed. Texel row 0 (the first bytes
  // in `framebuffer`) becomes texture coordinate t=0, matching
  // `framebuffer`'s own row 0 = top of the real IDisplay image, so the
  // quad below can use un-flipped texcoords.
  //
  // Storage is allocated once (glTexImage2D) and every subsequent frame
  // updates it in place (glTexSubImage2D) rather than reallocating --
  // repeatedly calling glTexImage2D at the ~30-60Hz this gets pushed at
  // is a known real driver footgun (can stall or transiently corrupt a
  // frame on some drivers); glTexSubImage2D is the correct, standard
  // pattern for a streaming texture that never changes size.
  if (first_time) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, /*border=*/0, GL_RGB,
                 GL_UNSIGNED_SHORT_5_6_5, framebuffer);
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                     framebuffer);
  }

  // Real app GL state (matrices, enables, ...) may currently hold
  // whatever the app itself last set -- save/restore around the quad so
  // presenting the 2D surface never corrupts real app-owned GL state.
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glEnable(GL_TEXTURE_2D);
  glViewport(0, 0, width_, height_);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrtho(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);  // (0,0) top-left, (1,1) bottom-right

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glBegin(GL_TRIANGLE_FAN);
  glTexCoord2f(0.0f, 0.0f);
  glVertex2f(0.0f, 0.0f);
  glTexCoord2f(1.0f, 0.0f);
  glVertex2f(1.0f, 0.0f);
  glTexCoord2f(1.0f, 1.0f);
  glVertex2f(1.0f, 1.0f);
  glTexCoord2f(0.0f, 1.0f);
  glVertex2f(0.0f, 1.0f);
  glEnd();

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glPopAttrib();

  PresentFrame();
}

// Drawn last, on top of whatever real content (2D quad above or the
// real app's own GLES draws via SwapBuffers) is already in the
// backbuffer for this frame -- same save/restore-state pattern as the
// PushVideoFrame quad above, so it never leaks state back to real app
// or video-quad rendering next frame.
void Sdl2UnifiedBackend::DrawFpsOverlay() {
  char text[16];
  std::snprintf(text, sizeof(text), "FPS:%d", static_cast<int>(fps_display_value_ + 0.5));

  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrtho(0.0, width_, height_, 0.0, -1.0, 1.0);  // (0,0) top-left, in real screen pixels

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  glColor4f(1.0f, 1.0f, 0.0f, 1.0f);

  constexpr int kPixel = 3;    // real screen pixels per font "pixel"
  constexpr int kGap = 1;      // real screen pixels between font pixels
  constexpr int kCharGap = 6;  // extra real screen pixels between glyphs
  constexpr int kOriginX = 6;
  constexpr int kOriginY = 6;

  int pen_x = kOriginX;
  for (const char* p = text; *p != '\0'; ++p) {
    const FontGlyph* glyph = FindGlyph(*p);
    if (glyph != nullptr) {
      for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 3; ++col) {
          if (((glyph->rows[row] >> (2 - col)) & 1) == 0) continue;
          int x = pen_x + col * (kPixel + kGap);
          int y = kOriginY + row * (kPixel + kGap);
          glBegin(GL_QUADS);
          glVertex2i(x, y);
          glVertex2i(x + kPixel, y);
          glVertex2i(x + kPixel, y + kPixel);
          glVertex2i(x, y + kPixel);
          glEnd();
        }
      }
    }
    pen_x += 3 * (kPixel + kGap) + kCharGap;
  }

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glPopAttrib();
}

// Real display refresh rate is capped well below 1000Hz, so a 500ms
// window is both frequent-enough to feel live and long-enough to
// average out real single-frame jitter.
void Sdl2UnifiedBackend::PresentFrame() {
  ++fps_frame_count_;
  Uint32 now = SDL_GetTicks();
  if (fps_last_tick_ms_ == 0) {
    fps_last_tick_ms_ = now;
  } else if (now - fps_last_tick_ms_ >= 500) {
    fps_display_value_ = fps_frame_count_ * 1000.0 / (now - fps_last_tick_ms_);
    fps_frame_count_ = 0;
    fps_last_tick_ms_ = now;
  }
  DrawFpsOverlay();
  SDL_GL_SwapWindow(window_);
}

void Sdl2UnifiedBackend::PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                                           int sample_rate) {
  if (audio_device_ == 0 || sample_rate != audio_sample_rate_) return;
  SDL_QueueAudio(audio_device_, interleaved_stereo,
                  static_cast<uint32_t>(frame_count * 2 * sizeof(int16_t)));
}

// Standard SDL_GameController button/axis naming already matches an
// Xbox-layout controller's own physical layout (A=bottom, B=right,
// X=left, Y=top), so this mapping *is* the "sane default matching a
// standard Xbox-layout controller" ARCHITECTURE.md 3.7 calls for, not
// an arbitrary choice on top of it.
ZPadState Sdl2UnifiedBackend::PollController() {
  ZPadState state;
  auto Set = [&](SDL_GameControllerButton button, uint16_t mask) {
    if (SDL_GameControllerGetButton(controller_, button)) state.buttons |= mask;
  };
  Set(SDL_CONTROLLER_BUTTON_DPAD_UP, ZPadState::kDpadUp);
  Set(SDL_CONTROLLER_BUTTON_DPAD_DOWN, ZPadState::kDpadDown);
  Set(SDL_CONTROLLER_BUTTON_DPAD_LEFT, ZPadState::kDpadLeft);
  Set(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ZPadState::kDpadRight);
  Set(SDL_CONTROLLER_BUTTON_START, ZPadState::kStartHome);
  Set(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ZPadState::kShoulderL);
  Set(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ZPadState::kShoulderR);
  Set(SDL_CONTROLLER_BUTTON_X, ZPadState::kButtonWest);
  Set(SDL_CONTROLLER_BUTTON_A, ZPadState::kButtonSouth);
  Set(SDL_CONTROLLER_BUTTON_Y, ZPadState::kButtonNorth);
  Set(SDL_CONTROLLER_BUTTON_B, ZPadState::kButtonEast);

  // SDL's axis range (-32768..32767) already matches ZPadState's
  // int16_t sticks directly -- no rescaling needed.
  state.left_stick_x = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX);
  state.left_stick_y = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY);
  state.right_stick_x = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTX);
  state.right_stick_y = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTY);
  return state;
}

// Fallback default when no real gamepad is connected: arrow keys for
// the D-pad, Z/X/A/S for the four face buttons (a common non-gamepad
// convention, e.g. RetroArch's own default keyboard binds), Q/E for the
// shoulder buttons, Return for Start/Home. Not derived from any real
// Zeebo device (there's no physical keyboard on one) -- purely this
// frontend's own dev/debug convenience, unlike the controller mapping
// above.
ZPadState Sdl2UnifiedBackend::PollKeyboard() {
  ZPadState state;
  const uint8_t* keys = SDL_GetKeyboardState(nullptr);
  auto Set = [&](SDL_Scancode key, uint16_t mask) {
    if (keys[key]) state.buttons |= mask;
  };
  Set(SDL_SCANCODE_UP, ZPadState::kDpadUp);
  Set(SDL_SCANCODE_DOWN, ZPadState::kDpadDown);
  Set(SDL_SCANCODE_LEFT, ZPadState::kDpadLeft);
  Set(SDL_SCANCODE_RIGHT, ZPadState::kDpadRight);
  Set(SDL_SCANCODE_RETURN, ZPadState::kStartHome);
  Set(SDL_SCANCODE_Q, ZPadState::kShoulderL);
  Set(SDL_SCANCODE_E, ZPadState::kShoulderR);
  Set(SDL_SCANCODE_A, ZPadState::kButtonWest);
  Set(SDL_SCANCODE_Z, ZPadState::kButtonSouth);
  Set(SDL_SCANCODE_S, ZPadState::kButtonNorth);
  Set(SDL_SCANCODE_X, ZPadState::kButtonEast);
  return state;
}

ZPadState Sdl2UnifiedBackend::PollInput() {
  return controller_ != nullptr ? PollController() : PollKeyboard();
}

bool Sdl2UnifiedBackend::CreateContext() { return gl_context_ != nullptr; }
void Sdl2UnifiedBackend::DestroyContext() {}

void Sdl2UnifiedBackend::SwapBuffers() {
  if (gl_context_ == nullptr) return;
  gl_swap_seen_ = true;
  SDL_GL_MakeCurrent(window_, gl_context_);
  PresentFrame();
}

void Sdl2UnifiedBackend::Clear(GLbitfield mask) { glClear(mask); }
void Sdl2UnifiedBackend::ClearColor(float r, float g, float b, float a) {
  glClearColor(r, g, b, a);
}
void Sdl2UnifiedBackend::Viewport(int x, int y, int width, int height) {
  glViewport(x, y, width, height);
}
void Sdl2UnifiedBackend::Enable(GLenum cap) { glEnable(cap); }
void Sdl2UnifiedBackend::Disable(GLenum cap) { glDisable(cap); }
void Sdl2UnifiedBackend::MatrixMode(GLenum mode) { glMatrixMode(mode); }
void Sdl2UnifiedBackend::LoadIdentity() { glLoadIdentity(); }
void Sdl2UnifiedBackend::Ortho(float left, float right, float bottom, float top,
                                float near_plane, float far_plane) {
  glOrtho(left, right, bottom, top, near_plane, far_plane);
}
void Sdl2UnifiedBackend::Frustum(float left, float right, float bottom, float top,
                                  float near_plane, float far_plane) {
  glFrustum(left, right, bottom, top, near_plane, far_plane);
}
void Sdl2UnifiedBackend::Translate(float x, float y, float z) { glTranslatef(x, y, z); }
void Sdl2UnifiedBackend::Rotate(float angle_degrees, float x, float y, float z) {
  glRotatef(angle_degrees, x, y, z);
}
void Sdl2UnifiedBackend::Scale(float x, float y, float z) { glScalef(x, y, z); }
void Sdl2UnifiedBackend::Color4(float r, float g, float b, float a) { glColor4f(r, g, b, a); }
void Sdl2UnifiedBackend::AlphaFunc(GLenum func, float ref) { glAlphaFunc(func, ref); }
void Sdl2UnifiedBackend::BlendFunc(GLenum sfactor, GLenum dfactor) { glBlendFunc(sfactor, dfactor); }

void Sdl2UnifiedBackend::DrawArrays(GLenum mode, const GlVertexArrays& arrays) {
  if (arrays.has_position) {
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(arrays.position_size, GL_FLOAT, 0, arrays.positions.data());
  } else {
    glDisableClientState(GL_VERTEX_ARRAY);
  }
  if (arrays.has_color) {
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, 0, arrays.colors.data());
  } else {
    glDisableClientState(GL_COLOR_ARRAY);
  }
  if (arrays.has_texcoord) {
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(arrays.texcoord_size, GL_FLOAT, 0, arrays.texcoords.data());
  } else {
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  }
  if (arrays.has_normal) {
    glEnableClientState(GL_NORMAL_ARRAY);
    glNormalPointer(GL_FLOAT, 0, arrays.normals.data());
  } else {
    glDisableClientState(GL_NORMAL_ARRAY);
  }
  glDrawArrays(mode, 0, arrays.vertex_count);
}

void Sdl2UnifiedBackend::GenTextures(GLsizei n, GLuint* textures) { glGenTextures(n, textures); }
void Sdl2UnifiedBackend::DeleteTextures(GLsizei n, const GLuint* textures) {
  glDeleteTextures(n, textures);
}
void Sdl2UnifiedBackend::BindTexture(GLenum target, GLuint texture) {
  glBindTexture(target, texture);
}
void Sdl2UnifiedBackend::TexParameter(GLenum target, GLenum pname, GLint param) {
  glTexParameteri(target, pname, param);
}
void Sdl2UnifiedBackend::TexImage2D(GLenum target, const GlTextureImage& image) {
  glTexImage2D(target, image.level, static_cast<GLint>(image.internal_format), image.width,
               image.height, /*border=*/0, image.format, image.type, image.pixels);
}

}  // namespace zeebulator
