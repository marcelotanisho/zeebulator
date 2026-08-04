#include "core/gl_texture_log.h"

#include <sstream>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

using zeebulator::BindTextureCall;
using zeebulator::DeleteTexturesCall;
using zeebulator::DeserializeGlTextureLog;
using zeebulator::GenTexturesCall;
using zeebulator::GLenum;
using zeebulator::GLint;
using zeebulator::GLsizei;
using zeebulator::GLuint;
using zeebulator::GlBackend;
using zeebulator::GlTextureImage;
using zeebulator::GlTextureLogEntry;
using zeebulator::GlTextureRecordingBackend;
using zeebulator::GlVertexArrays;
using zeebulator::ReplayGlTextureLog;
using zeebulator::SerializeGlTextureLog;
using zeebulator::TexImage2DCall;
using zeebulator::TexParameterCall;

namespace {

// Minimal fake covering exactly what these tests need to observe --
// real texture object bookkeeping (sequential ID assignment, matching
// every real desktop GL driver's own practical behavior from a fresh
// context -- see ReplayGlTextureLog's own doc comment) plus enough
// bound-texture/pixel tracking to verify a replay actually reproduced
// the recorded state. Every other GlBackend method is a no-op; nothing
// here exercises them.
class FakeGlBackend : public GlBackend {
 public:
  explicit FakeGlBackend(GLuint first_id = 1) : next_id_(first_id) {}

  bool CreateContext() override { return true; }
  void DestroyContext() override {}
  void SwapBuffers() override {}
  void Clear(zeebulator::GLbitfield) override {}
  void ClearColor(float, float, float, float) override {}
  void Viewport(int, int, int, int) override {}
  void Enable(GLenum) override {}
  void Disable(GLenum) override {}
  void MatrixMode(GLenum) override {}
  void LoadIdentity() override {}
  void PushMatrix() override {}
  void PopMatrix() override {}
  void Ortho(float, float, float, float, float, float) override {}
  void Frustum(float, float, float, float, float, float) override {}
  void Translate(float, float, float) override {}
  void Rotate(float, float, float, float) override {}
  void Scale(float, float, float) override {}
  void Color4(float, float, float, float) override {}
  void AlphaFunc(GLenum, float) override {}
  void BlendFunc(GLenum, GLenum) override {}
  void DepthFunc(GLenum) override {}
  void ClearDepth(float) override {}
  void DepthMask(bool) override {}
  void DrawArrays(GLenum, const GlVertexArrays&) override {}

  void GenTextures(GLsizei n, GLuint* textures) override {
    for (GLsizei i = 0; i < n; ++i) textures[i] = next_id_++;
  }
  void DeleteTextures(GLsizei n, const GLuint* textures) override {
    for (GLsizei i = 0; i < n; ++i) live_textures_.erase(textures[i]);
  }
  void BindTexture(GLenum, GLuint texture) override { bound_texture_ = texture; }
  void TexParameter(GLenum, GLenum, GLint) override {}
  void TexImage2D(GLenum, const GlTextureImage& image) override {
    std::vector<uint8_t> pixels;
    if (image.pixels != nullptr) {
      size_t count = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) *
                     static_cast<size_t>(zeebulator::GlPixelSize(image.format, image.type));
      pixels.assign(image.pixels, image.pixels + count);
    }
    live_textures_[bound_texture_] = std::move(pixels);
  }

  GLuint bound_texture_ = 0;
  std::unordered_map<GLuint, std::vector<uint8_t>> live_textures_;

 private:
  GLuint next_id_;
};

}  // namespace

TEST(GlTextureLog, RecordsGenTexturesWithTheRealAssignedIds) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);

  GLuint ids[2] = {0, 0};
  recorder.GenTextures(2, ids);

  ASSERT_EQ(recorder.Log().size(), 1u);
  const auto* call = std::get_if<GenTexturesCall>(&recorder.Log()[0]);
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->assigned_ids, (std::vector<GLuint>{1, 2}));
  EXPECT_EQ(ids[0], 1u);
  EXPECT_EQ(ids[1], 2u);
}

TEST(GlTextureLog, ClearLogDiscardsEverythingRecordedSoFar) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);  // simulates boot-time texture creation
  ASSERT_FALSE(recorder.Log().empty());

  recorder.ClearLog();
  EXPECT_TRUE(recorder.Log().empty());

  // Recording resumes normally afterward.
  recorder.BindTexture(0x0DE1, id);
  EXPECT_EQ(recorder.Log().size(), 1u);
}

TEST(GlTextureLog, RecordsTexImage2DPixelDataByValueNotByPointer) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1 /* GL_TEXTURE_2D-ish */, id);

  std::vector<uint16_t> pixels = {0x1234, 0x5678};  // 2x1 RGB565-shaped
  GlTextureImage image;
  image.width = 2;
  image.height = 1;
  image.format = 0x1907;  // arbitrary stand-in, not checked by the fake
  image.type = 0x8363;
  image.pixels = reinterpret_cast<const uint8_t*>(pixels.data());
  recorder.TexImage2D(0x0DE1, image);

  // Mutate the original buffer -- the recorded copy must be unaffected,
  // matching GlTextureImage's own doc comment that `pixels` is only
  // valid for the call's own duration.
  pixels[0] = 0xFFFF;

  ASSERT_EQ(recorder.Log().size(), 3u);
  const auto* tex_call = std::get_if<TexImage2DCall>(&recorder.Log()[2]);
  ASSERT_NE(tex_call, nullptr);
  ASSERT_EQ(tex_call->pixels.size(), 4u);
  EXPECT_EQ(tex_call->pixels[0], 0x34);
  EXPECT_EQ(tex_call->pixels[1], 0x12);
}

TEST(GlTextureLog, SerializeThenDeserializeRoundTrips) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1, id);
  recorder.TexParameter(0x0DE1, 0x2801, 0x2600);
  uint8_t pixel_bytes[4] = {0x11, 0x22, 0x33, 0x44};
  GlTextureImage image;
  image.width = 2;
  image.height = 1;
  image.type = 0x8363;  // kGlUnsignedShort565 -- 2 bytes/pixel, matches pixel_bytes' size
  image.pixels = pixel_bytes;
  recorder.TexImage2D(0x0DE1, image);
  GLuint delete_id = id;
  recorder.DeleteTextures(1, &delete_id);

  std::stringstream stream;
  ASSERT_TRUE(SerializeGlTextureLog(recorder.Log(), stream));

  std::vector<GlTextureLogEntry> restored;
  ASSERT_TRUE(DeserializeGlTextureLog(stream, restored));
  ASSERT_EQ(restored.size(), recorder.Log().size());

  ASSERT_TRUE(std::holds_alternative<GenTexturesCall>(restored[0]));
  EXPECT_EQ(std::get<GenTexturesCall>(restored[0]).assigned_ids, (std::vector<GLuint>{1}));

  ASSERT_TRUE(std::holds_alternative<BindTextureCall>(restored[1]));
  EXPECT_EQ(std::get<BindTextureCall>(restored[1]).texture, 1u);

  ASSERT_TRUE(std::holds_alternative<TexParameterCall>(restored[2]));
  EXPECT_EQ(std::get<TexParameterCall>(restored[2]).param, 0x2600);

  ASSERT_TRUE(std::holds_alternative<TexImage2DCall>(restored[3]));
  const auto& tex = std::get<TexImage2DCall>(restored[3]);
  EXPECT_TRUE(tex.has_pixels);
  ASSERT_EQ(tex.pixels.size(), 4u);
  EXPECT_EQ(tex.pixels[0], 0x11);
  EXPECT_EQ(tex.pixels[3], 0x44);

  ASSERT_TRUE(std::holds_alternative<DeleteTexturesCall>(restored[4]));
  EXPECT_EQ(std::get<DeleteTexturesCall>(restored[4]).ids, (std::vector<GLuint>{1}));
}

TEST(GlTextureLog, ReplayReproducesTheSameTexturesOnAFreshBackend) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);
  recorder.BindTexture(0x0DE1, id);
  uint8_t pixel_bytes[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  GlTextureImage image;
  image.width = 2;
  image.height = 1;
  image.type = 0x8363;  // kGlUnsignedShort565 -- 2 bytes/pixel, matches pixel_bytes' size
  image.pixels = pixel_bytes;
  recorder.TexImage2D(0x0DE1, image);

  FakeGlBackend fresh;  // a second, independent "cold" backend
  bool ok = ReplayGlTextureLog(recorder.Log(), fresh);
  EXPECT_TRUE(ok);

  ASSERT_EQ(fresh.live_textures_.count(1u), 1u);
  const std::vector<uint8_t>& replayed_pixels = fresh.live_textures_.at(1u);
  ASSERT_EQ(replayed_pixels.size(), 4u);
  EXPECT_EQ(replayed_pixels[0], 0xAA);
  EXPECT_EQ(replayed_pixels[3], 0xDD);
}

TEST(GlTextureLog, ReplayReportsFailureWhenTheTargetAssignsDifferentIds) {
  FakeGlBackend real;
  GlTextureRecordingBackend recorder(real);
  GLuint id = 0;
  recorder.GenTextures(1, &id);  // recorded as ID 1

  FakeGlBackend mismatched(/*first_id=*/50);  // will assign ID 50, not 1
  EXPECT_FALSE(ReplayGlTextureLog(recorder.Log(), mismatched));
}
