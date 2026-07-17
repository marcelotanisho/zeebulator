#include "core/brew/gl_hle.h"

#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {

void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }

// EGL sentinel handles: this class never talks to any real host EGL
// implementation (GlBackend owns the one real GL context), so these just
// need to be stable non-zero values the app can pass back into later
// calls (eglMakeCurrent, eglDestroySurface, ...) without us caring what
// they "mean".
constexpr EGLHandle kDisplayHandle = 1;
constexpr EGLHandle kConfigHandle = 1;
constexpr EGLHandle kSurfaceHandle = 1;
constexpr EGLHandle kContextHandle = 1;

// EGL_SUCCESS, per the standard Khronos EGL error-code namespace (a
// stable industry constant, not Qualcomm-specific text).
constexpr EGLint kEglSuccess = 0x3000;

void WriteEGLintIfNonNull(Memory& memory, uint32_t addr, EGLint value) {
  if (addr != 0) {
    memory.Write32(addr, static_cast<uint32_t>(value));
  }
}

// Scratch space for eglQueryString's return value -- real callers only
// ever read the string immediately after the call (confirmed via real
// disassembly of Double Dragon, TASKS.md Phase 8: fed straight into a
// strstr-shaped call), so a single reused buffer is enough.
constexpr uint32_t kQueryStringBufferAddr = 0x8001B000;

void WriteCString(Memory& memory, uint32_t addr, const char* text) {
  size_t i = 0;
  for (; text[i] != '\0'; ++i) {
    memory.Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  memory.Write8(addr + static_cast<uint32_t>(i), 0);
}

}  // namespace

GlHle::GlHle(GlBackend& backend) : backend_(backend) {}

// --- EGL lifecycle -----------------------------------------------------
// None of these slots receive the interface pointer in R0 (see gl_hle.h)
// -- R0 holds the first *declared* argument.

void GlHle::EglGetError(IArmCore& core) { core.SetRegister(kR0, static_cast<uint32_t>(kEglSuccess)); }

void GlHle::EglGetDisplay(IArmCore& core) {
  // EGLDisplay eglGetDisplay(NativeDisplayType display) -- the real arg
  // (R0) is ignored; we always hand back the one simulated display.
  core.SetRegister(kR0, kDisplayHandle);
}

void GlHle::EglInitialize(IArmCore& core) {
  // EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
  WriteEGLintIfNonNull(core.GetMemory(), core.GetRegister(kR1), 1);
  WriteEGLintIfNonNull(core.GetMemory(), core.GetRegister(kR2), 0);
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglQueryString(IArmCore& core) {
  // const char *eglQueryString(EGLDisplay dpy, EGLint name) -- R0 is dpy
  // (ignored), R1 is name. Real EGL 1.x values (confirmed against the
  // real BREW OpenGL ES extension SDK headers, extracted from
  // research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/
  // OpenGLES_Extension_...zip -- see TASKS.md Phase 8).
  constexpr EGLint kEglVendor = 0x3053;
  constexpr EGLint kEglVersion = 0x3054;
  constexpr EGLint kEglClientApis = 0x308D;
  auto name = static_cast<EGLint>(core.GetRegister(kR1));
  const char* value = "";  // EGL_EXTENSIONS and anything unrecognized: no
                            // extensions implemented, an honest empty
                            // string, never null (see class doc comment).
  if (name == kEglVendor) {
    value = "Zeebulator";
  } else if (name == kEglVersion) {
    value = "1.0";
  } else if (name == kEglClientApis) {
    value = "OpenGL_ES";
  }
  WriteCString(core.GetMemory(), kQueryStringBufferAddr, value);
  core.SetRegister(kR0, kQueryStringBufferAddr);
}

void GlHle::EglTerminate(IArmCore& core) {
  if (context_current_) {
    backend_.DestroyContext();
    context_current_ = false;
  }
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglChooseConfig(IArmCore& core) {
  // EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
  //                            EGLConfig *configs, EGLint config_size,
  //                            EGLint *num_config)
  uint32_t configs = core.GetRegister(kR2);
  auto config_size = static_cast<int32_t>(core.GetRegister(kR3));
  uint32_t num_config = HleRuntime::ReadStackArg(core, 0);
  if (configs != 0 && config_size >= 1) {
    core.GetMemory().Write32(configs, kConfigHandle);
  }
  WriteEGLintIfNonNull(core.GetMemory(), num_config, 1);
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglCreateWindowSurface(IArmCore& core) {
  core.SetRegister(kR0, kSurfaceHandle);
}

void GlHle::EglDestroySurface(IArmCore& core) { core.SetRegister(kR0, kEglTrue); }

void GlHle::EglCreateContext(IArmCore& core) { core.SetRegister(kR0, kContextHandle); }

void GlHle::EglDestroyContext(IArmCore& core) {
  if (context_current_) {
    backend_.DestroyContext();
    context_current_ = false;
  }
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglMakeCurrent(IArmCore& core) {
  // EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
  //                           EGLSurface read, EGLContext ctx)
  uint32_t ctx = core.GetRegister(kR3);
  if (ctx != 0 && !context_current_) {
    context_current_ = backend_.CreateContext();
  } else if (ctx == 0) {
    context_current_ = false;
  }
  core.SetRegister(kR0, kEglTrue);
}

void GlHle::EglSwapBuffers(IArmCore& core) {
  backend_.SwapBuffers();
  core.SetRegister(kR0, kEglTrue);
}

// --- Core GL state / transform ------------------------------------------

void GlHle::GlClear(IArmCore& core) {
  backend_.Clear(core.GetRegister(kR0));
}

void GlHle::GlClearColorx(IArmCore& core) {
  backend_.ClearColor(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                       FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                       FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))),
                       FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR3))));
}

void GlHle::GlViewport(IArmCore& core) {
  backend_.Viewport(static_cast<int>(core.GetRegister(kR0)),
                     static_cast<int>(core.GetRegister(kR1)),
                     static_cast<int>(core.GetRegister(kR2)),
                     static_cast<int>(core.GetRegister(kR3)));
}

void GlHle::GlEnable(IArmCore& core) { backend_.Enable(core.GetRegister(kR0)); }
void GlHle::GlDisable(IArmCore& core) { backend_.Disable(core.GetRegister(kR0)); }
void GlHle::GlMatrixMode(IArmCore& core) { backend_.MatrixMode(core.GetRegister(kR0)); }
void GlHle::GlLoadIdentity(IArmCore&) { backend_.LoadIdentity(); }

void GlHle::GlOrthox(IArmCore& core) {
  backend_.Ortho(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                  FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                  FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))),
                  FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR3))),
                  FixedToFloat(static_cast<GLfixed>(HleRuntime::ReadStackArg(core, 0))),
                  FixedToFloat(static_cast<GLfixed>(HleRuntime::ReadStackArg(core, 1))));
}

void GlHle::GlFrustumx(IArmCore& core) {
  backend_.Frustum(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                    FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                    FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))),
                    FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR3))),
                    FixedToFloat(static_cast<GLfixed>(HleRuntime::ReadStackArg(core, 0))),
                    FixedToFloat(static_cast<GLfixed>(HleRuntime::ReadStackArg(core, 1))));
}

void GlHle::GlTranslatex(IArmCore& core) {
  backend_.Translate(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                      FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                      FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))));
}

void GlHle::GlRotatex(IArmCore& core) {
  backend_.Rotate(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR3))));
}

void GlHle::GlScalex(IArmCore& core) {
  backend_.Scale(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                  FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                  FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))));
}

void GlHle::GlColor4x(IArmCore& core) {
  backend_.Color4(FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR0))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR1))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR2))),
                   FixedToFloat(static_cast<GLfixed>(core.GetRegister(kR3))));
}

// --- Vertex arrays / draw calls -------------------------------------------

void GlHle::GlVertexPointer(IArmCore& core) {
  // void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
  vertex_array_.size = static_cast<int>(core.GetRegister(kR0));
  vertex_array_.type = core.GetRegister(kR1);
  vertex_array_.stride = static_cast<int>(core.GetRegister(kR2));
  vertex_array_.pointer = core.GetRegister(kR3);
}

void GlHle::GlColorPointer(IArmCore& core) {
  // void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
  color_array_.size = static_cast<int>(core.GetRegister(kR0));
  color_array_.type = core.GetRegister(kR1);
  color_array_.stride = static_cast<int>(core.GetRegister(kR2));
  color_array_.pointer = core.GetRegister(kR3);
}

void GlHle::GlTexCoordPointer(IArmCore& core) {
  // void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
  texcoord_array_.size = static_cast<int>(core.GetRegister(kR0));
  texcoord_array_.type = core.GetRegister(kR1);
  texcoord_array_.stride = static_cast<int>(core.GetRegister(kR2));
  texcoord_array_.pointer = core.GetRegister(kR3);
}

void GlHle::GlNormalPointer(IArmCore& core) {
  // void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer)
  // -- no size argument, a normal is always 3 components.
  normal_array_.size = 3;
  normal_array_.type = core.GetRegister(kR0);
  normal_array_.stride = static_cast<int>(core.GetRegister(kR1));
  normal_array_.pointer = core.GetRegister(kR2);
}

void GlHle::GlEnableClientState(IArmCore& core) {
  // void glEnableClientState(GLenum array)
  switch (core.GetRegister(kR0)) {
    case kGlVertexArray: vertex_array_.enabled = true; break;
    case kGlColorArray: color_array_.enabled = true; break;
    case kGlTextureCoordArray: texcoord_array_.enabled = true; break;
    case kGlNormalArray: normal_array_.enabled = true; break;
    default: break;
  }
}

void GlHle::GlDisableClientState(IArmCore& core) {
  // void glDisableClientState(GLenum array)
  switch (core.GetRegister(kR0)) {
    case kGlVertexArray: vertex_array_.enabled = false; break;
    case kGlColorArray: color_array_.enabled = false; break;
    case kGlTextureCoordArray: texcoord_array_.enabled = false; break;
    case kGlNormalArray: normal_array_.enabled = false; break;
    default: break;
  }
}

GlVertexArrays GlHle::ExtractArrays(Memory& memory,
                                     const std::vector<uint32_t>& indices) const {
  GlVertexArrays out;
  out.vertex_count = static_cast<int>(indices.size());

  auto extract = [&](const ArrayState& array, std::vector<float>& dest, bool normalize_ubyte) {
    int component_bytes = GlTypeSize(array.type);
    int stride = array.stride != 0 ? array.stride : array.size * component_bytes;
    dest.reserve(dest.size() + indices.size() * static_cast<size_t>(array.size));
    for (uint32_t index : indices) {
      uint32_t base = array.pointer + index * static_cast<uint32_t>(stride);
      for (int c = 0; c < array.size; ++c) {
        float value = ReadGlComponent(memory, base + static_cast<uint32_t>(c * component_bytes),
                                       array.type);
        if (normalize_ubyte && array.type == kGlUnsignedByte) {
          value /= 255.0f;
        }
        dest.push_back(value);
      }
    }
  };

  if (vertex_array_.enabled) {
    out.has_position = true;
    out.position_size = vertex_array_.size;
    extract(vertex_array_, out.positions, false);
  }
  if (color_array_.enabled) {
    out.has_color = true;
    extract(color_array_, out.colors, true);
  }
  if (texcoord_array_.enabled) {
    out.has_texcoord = true;
    out.texcoord_size = texcoord_array_.size;
    extract(texcoord_array_, out.texcoords, false);
  }
  if (normal_array_.enabled) {
    out.has_normal = true;
    extract(normal_array_, out.normals, false);
  }
  return out;
}

void GlHle::GlDrawArrays(IArmCore& core) {
  // void glDrawArrays(GLenum mode, GLint first, GLsizei count)
  GLenum mode = core.GetRegister(kR0);
  auto first = static_cast<int32_t>(core.GetRegister(kR1));
  auto count = static_cast<int32_t>(core.GetRegister(kR2));

  std::vector<uint32_t> indices;
  indices.reserve(static_cast<size_t>(count > 0 ? count : 0));
  for (int32_t i = 0; i < count; ++i) {
    indices.push_back(static_cast<uint32_t>(first + i));
  }
  backend_.DrawArrays(mode, ExtractArrays(core.GetMemory(), indices));
}

void GlHle::GlDrawElements(IArmCore& core) {
  // void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
  GLenum mode = core.GetRegister(kR0);
  auto count = static_cast<int32_t>(core.GetRegister(kR1));
  GLenum type = core.GetRegister(kR2);
  uint32_t indices_ptr = core.GetRegister(kR3);

  std::vector<uint32_t> indices;
  indices.reserve(static_cast<size_t>(count > 0 ? count : 0));
  Memory& memory = core.GetMemory();
  for (int32_t i = 0; i < count; ++i) {
    uint32_t index = (type == kGlUnsignedShort)
                          ? memory.Read16(indices_ptr + static_cast<uint32_t>(i) * 2)
                          : memory.Read8(indices_ptr + static_cast<uint32_t>(i));
    indices.push_back(index);
  }
  backend_.DrawArrays(mode, ExtractArrays(memory, indices));
}

// --- Texture object management + upload -----------------------------------

void GlHle::GlGenTextures(IArmCore& core) {
  // void glGenTextures(GLsizei n, GLuint *textures)
  auto n = static_cast<int32_t>(core.GetRegister(kR0));
  uint32_t textures_ptr = core.GetRegister(kR1);
  if (n <= 0) return;

  std::vector<GLuint> textures(static_cast<size_t>(n), 0);
  backend_.GenTextures(n, textures.data());
  Memory& memory = core.GetMemory();
  for (int32_t i = 0; i < n; ++i) {
    memory.Write32(textures_ptr + static_cast<uint32_t>(i) * 4, textures[static_cast<size_t>(i)]);
  }
}

void GlHle::GlDeleteTextures(IArmCore& core) {
  // void glDeleteTextures(GLsizei n, const GLuint *textures)
  auto n = static_cast<int32_t>(core.GetRegister(kR0));
  uint32_t textures_ptr = core.GetRegister(kR1);
  if (n <= 0) return;

  std::vector<GLuint> textures(static_cast<size_t>(n));
  Memory& memory = core.GetMemory();
  for (int32_t i = 0; i < n; ++i) {
    textures[static_cast<size_t>(i)] = memory.Read32(textures_ptr + static_cast<uint32_t>(i) * 4);
  }
  backend_.DeleteTextures(n, textures.data());
}

void GlHle::GlBindTexture(IArmCore& core) {
  backend_.BindTexture(core.GetRegister(kR0), core.GetRegister(kR1));
}

void GlHle::GlTexParameterx(IArmCore& core) {
  // void glTexParameterx(GLenum target, GLenum pname, GLfixed param) --
  // param is the raw enum integer, not a true fixed-point value (see
  // GlBackend::TexParameter's comment).
  backend_.TexParameter(core.GetRegister(kR0), core.GetRegister(kR1),
                         static_cast<GLint>(core.GetRegister(kR2)));
}

void GlHle::GlTexImage2D(IArmCore& core) {
  // void glTexImage2D(GLenum target, GLint level, GLint internalformat,
  //                    GLsizei width, GLsizei height, GLint border,
  //                    GLenum format, GLenum type, const GLvoid *pixels)
  GLenum target = core.GetRegister(kR0);
  GlTextureImage image;
  image.level = static_cast<int>(core.GetRegister(kR1));
  image.internal_format = core.GetRegister(kR2);
  image.width = static_cast<int>(core.GetRegister(kR3));
  image.height = static_cast<int>(HleRuntime::ReadStackArg(core, 0));
  // border (stack arg 1) is always 0 per the GLES1.x spec -- not
  // forwarded, GlBackend has nothing meaningful to do with it.
  image.format = HleRuntime::ReadStackArg(core, 2);
  image.type = HleRuntime::ReadStackArg(core, 3);
  uint32_t pixels_ptr = HleRuntime::ReadStackArg(core, 4);

  std::vector<uint8_t> pixel_bytes;
  if (pixels_ptr != 0 && image.width > 0 && image.height > 0) {
    size_t total = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) *
                    static_cast<size_t>(GlPixelSize(image.format, image.type));
    pixel_bytes.resize(total);
    Memory& memory = core.GetMemory();
    for (size_t i = 0; i < total; ++i) {
      pixel_bytes[i] = memory.Read8(pixels_ptr + static_cast<uint32_t>(i));
    }
    image.pixels = pixel_bytes.data();
  }
  backend_.TexImage2D(target, image);
}

// --- Vtable construction -------------------------------------------------

uint32_t GlHle::BuildGl(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                         uint32_t object_address) {
  // Slot order verified directly against the real AEEGL.h (extracted from
  // a genuine Qualcomm "OpenGL ES Extension for BREW SDK 4.x" installer --
  // see TASKS.md Phase 5). 80 slots total: AddRef/Release/QueryInterface,
  // then the 77 gl* methods in AEEGL.h's exact declared order.
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                                       // 0  AddRef
      Stub,                                       // 1  Release
      Stub,                                       // 2  QueryInterface
      Stub,                                       // 3  glActiveTexture
      Stub,                                       // 4  glAlphaFuncx
      [this](IArmCore& c) { GlBindTexture(c); },  // 5  glBindTexture
      Stub,                                       // 6  glBlendFunc
      [this](IArmCore& c) { GlClear(c); },        // 7  glClear
      [this](IArmCore& c) { GlClearColorx(c); },  // 8  glClearColorx
      Stub,                                       // 9  glClearDepthx
      Stub,                                       // 10 glClearStencil
      Stub,                                       // 11 glClientActiveTexture
      [this](IArmCore& c) { GlColor4x(c); },      // 12 glColor4x
      Stub,                                       // 13 glColorMask
      [this](IArmCore& c) { GlColorPointer(c); }, // 14 glColorPointer
      Stub,                                       // 15 glCompressedTexImage2D
      Stub,                                       // 16 glCompressedTexSubImage2D
      Stub,                                       // 17 glCopyTexImage2D
      Stub,                                       // 18 glCopyTexSubImage2D
      Stub,                                       // 19 glCullFace
      [this](IArmCore& c) { GlDeleteTextures(c); }, // 20 glDeleteTextures
      Stub,                                       // 21 glDepthFunc
      Stub,                                       // 22 glDepthMask
      Stub,                                       // 23 glDepthRangex
      [this](IArmCore& c) { GlDisable(c); },      // 24 glDisable
      [this](IArmCore& c) { GlDisableClientState(c); },  // 25 glDisableClientState
      [this](IArmCore& c) { GlDrawArrays(c); },   // 26 glDrawArrays
      [this](IArmCore& c) { GlDrawElements(c); }, // 27 glDrawElements
      [this](IArmCore& c) { GlEnable(c); },       // 28 glEnable
      [this](IArmCore& c) { GlEnableClientState(c); },   // 29 glEnableClientState
      Stub,                                       // 30 glFinish
      Stub,                                       // 31 glFlush
      Stub,                                       // 32 glFogx
      Stub,                                       // 33 glFogxv
      Stub,                                       // 34 glFrontFace
      [this](IArmCore& c) { GlFrustumx(c); },     // 35 glFrustumx
      [this](IArmCore& c) { GlGenTextures(c); },  // 36 glGenTextures
      Stub,                                       // 37 glGetError
      Stub,                                       // 38 glGetIntegerv
      Stub,                                       // 39 glGetString
      Stub,                                       // 40 glHint
      Stub,                                       // 41 glLightModelx
      Stub,                                       // 42 glLightModelxv
      Stub,                                       // 43 glLightx
      Stub,                                       // 44 glLightxv
      Stub,                                       // 45 glLineWidthx
      [this](IArmCore& c) { GlLoadIdentity(c); }, // 46 glLoadIdentity
      Stub,                                       // 47 glLoadMatrixx
      Stub,                                       // 48 glLogicOp
      Stub,                                       // 49 glMaterialx
      Stub,                                       // 50 glMaterialxv
      [this](IArmCore& c) { GlMatrixMode(c); },   // 51 glMatrixMode
      Stub,                                       // 52 glMultMatrixx
      Stub,                                       // 53 glMultiTexCoord4x
      Stub,                                       // 54 glNormal3x
      [this](IArmCore& c) { GlNormalPointer(c); }, // 55 glNormalPointer
      [this](IArmCore& c) { GlOrthox(c); },       // 56 glOrthox
      Stub,                                       // 57 glPixelStorei
      Stub,                                       // 58 glPointSizex
      Stub,                                       // 59 glPolygonOffsetx
      Stub,                                       // 60 glPopMatrix
      Stub,                                       // 61 glPushMatrix
      Stub,                                       // 62 glReadPixels
      [this](IArmCore& c) { GlRotatex(c); },      // 63 glRotatex
      Stub,                                       // 64 glSampleCoveragex
      [this](IArmCore& c) { GlScalex(c); },       // 65 glScalex
      Stub,                                       // 66 glScissor
      Stub,                                       // 67 glShadeModel
      Stub,                                       // 68 glStencilFunc
      Stub,                                       // 69 glStencilMask
      Stub,                                       // 70 glStencilOp
      [this](IArmCore& c) { GlTexCoordPointer(c); }, // 71 glTexCoordPointer
      Stub,                                       // 72 glTexEnvx
      Stub,                                       // 73 glTexEnvxv
      [this](IArmCore& c) { GlTexImage2D(c); },   // 74 glTexImage2D
      [this](IArmCore& c) { GlTexParameterx(c); }, // 75 glTexParameterx
      Stub,                                       // 76 glTexSubImage2D
      [this](IArmCore& c) { GlTranslatex(c); },   // 77 glTranslatex
      [this](IArmCore& c) { GlVertexPointer(c); }, // 78 glVertexPointer
      [this](IArmCore& c) { GlViewport(c); },     // 79 glViewport
  };
  return BuildInterfaceObject(memory, hle, vtable_address, object_address, methods);
}

uint32_t GlHle::BuildEgl(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                          uint32_t object_address) {
  // 28 slots total: AddRef/Release/QueryInterface, then the 25 egl*
  // methods in AEEGL.h's exact declared order.
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                                                // 0  AddRef
      Stub,                                                // 1  Release
      Stub,                                                // 2  QueryInterface
      [this](IArmCore& c) { EglGetError(c); },              // 3  eglGetError
      [this](IArmCore& c) { EglGetDisplay(c); },             // 4  eglGetDisplay
      [this](IArmCore& c) { EglInitialize(c); },             // 5  eglInitialize
      [this](IArmCore& c) { EglTerminate(c); },              // 6  eglTerminate
      [this](IArmCore& c) { EglQueryString(c); },            // 7  eglQueryString
      Stub,                                                // 8  eglGetProcAddress
      Stub,                                                // 9  eglGetConfigs
      [this](IArmCore& c) { EglChooseConfig(c); },           // 10 eglChooseConfig
      Stub,                                                // 11 eglGetConfigAttrib
      [this](IArmCore& c) { EglCreateWindowSurface(c); },    // 12 eglCreateWindowSurface
      Stub,                                                // 13 eglCreatePixmapSurface
      Stub,                                                // 14 eglCreatePbufferSurface
      [this](IArmCore& c) { EglDestroySurface(c); },         // 15 eglDestroySurface
      Stub,                                                // 16 eglQuerySurface
      [this](IArmCore& c) { EglCreateContext(c); },          // 17 eglCreateContext
      [this](IArmCore& c) { EglDestroyContext(c); },         // 18 eglDestroyContext
      [this](IArmCore& c) { EglMakeCurrent(c); },            // 19 eglMakeCurrent
      Stub,                                                // 20 eglGetCurrentContext
      Stub,                                                // 21 eglGetCurrentSurface
      Stub,                                                // 22 eglGetCurrentDisplay
      Stub,                                                // 23 eglQueryContext
      Stub,                                                // 24 eglWaitGL
      Stub,                                                // 25 eglWaitNative
      [this](IArmCore& c) { EglSwapBuffers(c); },            // 26 eglSwapBuffers
      Stub,                                                // 27 eglCopyBuffers
  };
  return BuildInterfaceObject(memory, hle, vtable_address, object_address, methods);
}

}  // namespace zeebulator
