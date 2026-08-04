#include "core/gl_texture_log.h"

#include <cstdio>

namespace zeebulator {

void GlTextureRecordingBackend::GenTextures(GLsizei n, GLuint* textures) {
  real_.GenTextures(n, textures);
  GenTexturesCall call;
  call.assigned_ids.assign(textures, textures + n);
  log_.push_back(std::move(call));
}

void GlTextureRecordingBackend::DeleteTextures(GLsizei n, const GLuint* textures) {
  DeleteTexturesCall call;
  call.ids.assign(textures, textures + n);
  log_.push_back(std::move(call));
  real_.DeleteTextures(n, textures);
}

void GlTextureRecordingBackend::BindTexture(GLenum target, GLuint texture) {
  log_.push_back(BindTextureCall{target, texture});
  real_.BindTexture(target, texture);
}

void GlTextureRecordingBackend::TexParameter(GLenum target, GLenum pname, GLint param) {
  log_.push_back(TexParameterCall{target, pname, param});
  real_.TexParameter(target, pname, param);
}

void GlTextureRecordingBackend::TexImage2D(GLenum target, const GlTextureImage& image) {
  TexImage2DCall call;
  call.target = target;
  call.level = image.level;
  call.internal_format = image.internal_format;
  call.width = image.width;
  call.height = image.height;
  call.format = image.format;
  call.type = image.type;
  call.has_pixels = image.pixels != nullptr;
  if (call.has_pixels && image.width > 0 && image.height > 0) {
    size_t total = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) *
                    static_cast<size_t>(GlPixelSize(image.format, image.type));
    call.pixels.assign(image.pixels, image.pixels + total);
  }
  log_.push_back(std::move(call));
  real_.TexImage2D(target, image);
}

bool ReplayGlTextureLog(const std::vector<GlTextureLogEntry>& log, GlBackend& target) {
  bool all_ids_matched = true;
  for (const GlTextureLogEntry& entry : log) {
    if (const auto* gen = std::get_if<GenTexturesCall>(&entry)) {
      std::vector<GLuint> assigned(gen->assigned_ids.size());
      target.GenTextures(static_cast<GLsizei>(assigned.size()), assigned.data());
      if (assigned != gen->assigned_ids) {
        all_ids_matched = false;
        std::fprintf(stderr,
                     "ReplayGlTextureLog: real texture ID assignment didn't reproduce the "
                     "recorded sequence (driver-dependent, see this function's own doc "
                     "comment) -- replayed textures will be wrong\n");
      }
    } else if (const auto* del = std::get_if<DeleteTexturesCall>(&entry)) {
      target.DeleteTextures(static_cast<GLsizei>(del->ids.size()), del->ids.data());
    } else if (const auto* bind = std::get_if<BindTextureCall>(&entry)) {
      target.BindTexture(bind->target, bind->texture);
    } else if (const auto* param = std::get_if<TexParameterCall>(&entry)) {
      target.TexParameter(param->target, param->pname, param->param);
    } else if (const auto* tex = std::get_if<TexImage2DCall>(&entry)) {
      GlTextureImage image;
      image.level = tex->level;
      image.internal_format = tex->internal_format;
      image.width = tex->width;
      image.height = tex->height;
      image.format = tex->format;
      image.type = tex->type;
      image.pixels = tex->has_pixels ? tex->pixels.data() : nullptr;
      target.TexImage2D(tex->target, image);
    }
  }
  return all_ids_matched;
}

namespace {

// Tags identifying each GlTextureLogEntry variant alternative on disk --
// this project's own numbering, not a real GL value, so log_entry_kind
// doesn't need to survive std::variant's own index() changing if entry
// types are ever reordered in the header.
enum class LogEntryKind : uint8_t {
  kGenTextures = 0,
  kDeleteTextures = 1,
  kBindTexture = 2,
  kTexParameter = 3,
  kTexImage2D = 4,
};

template <typename T>
bool WritePod(std::ostream& out, const T& value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(T));
  return out.good();
}

template <typename T>
bool ReadPod(std::istream& in, T& value) {
  in.read(reinterpret_cast<char*>(&value), sizeof(T));
  return in.good();
}

bool WriteGLuintVector(std::ostream& out, const std::vector<GLuint>& values) {
  uint32_t count = static_cast<uint32_t>(values.size());
  if (!WritePod(out, count)) return false;
  if (count == 0) return true;
  out.write(reinterpret_cast<const char*>(values.data()),
            static_cast<std::streamsize>(values.size() * sizeof(GLuint)));
  return out.good();
}

bool ReadGLuintVector(std::istream& in, std::vector<GLuint>& values) {
  uint32_t count = 0;
  if (!ReadPod(in, count)) return false;
  values.resize(count);
  if (count == 0) return true;
  in.read(reinterpret_cast<char*>(values.data()),
          static_cast<std::streamsize>(values.size() * sizeof(GLuint)));
  return in.good();
}

}  // namespace

bool SerializeGlTextureLog(const std::vector<GlTextureLogEntry>& log, std::ostream& out) {
  uint32_t entry_count = static_cast<uint32_t>(log.size());
  if (!WritePod(out, entry_count)) return false;

  for (const GlTextureLogEntry& entry : log) {
    if (const auto* gen = std::get_if<GenTexturesCall>(&entry)) {
      if (!WritePod(out, LogEntryKind::kGenTextures)) return false;
      if (!WriteGLuintVector(out, gen->assigned_ids)) return false;
    } else if (const auto* del = std::get_if<DeleteTexturesCall>(&entry)) {
      if (!WritePod(out, LogEntryKind::kDeleteTextures)) return false;
      if (!WriteGLuintVector(out, del->ids)) return false;
    } else if (const auto* bind = std::get_if<BindTextureCall>(&entry)) {
      if (!WritePod(out, LogEntryKind::kBindTexture)) return false;
      if (!WritePod(out, bind->target)) return false;
      if (!WritePod(out, bind->texture)) return false;
    } else if (const auto* param = std::get_if<TexParameterCall>(&entry)) {
      if (!WritePod(out, LogEntryKind::kTexParameter)) return false;
      if (!WritePod(out, param->target)) return false;
      if (!WritePod(out, param->pname)) return false;
      if (!WritePod(out, param->param)) return false;
    } else if (const auto* tex = std::get_if<TexImage2DCall>(&entry)) {
      if (!WritePod(out, LogEntryKind::kTexImage2D)) return false;
      if (!WritePod(out, tex->target)) return false;
      if (!WritePod(out, tex->level)) return false;
      if (!WritePod(out, tex->internal_format)) return false;
      if (!WritePod(out, tex->width)) return false;
      if (!WritePod(out, tex->height)) return false;
      if (!WritePod(out, tex->format)) return false;
      if (!WritePod(out, tex->type)) return false;
      if (!WritePod(out, tex->has_pixels)) return false;
      uint32_t pixel_count = static_cast<uint32_t>(tex->pixels.size());
      if (!WritePod(out, pixel_count)) return false;
      if (pixel_count != 0) {
        out.write(reinterpret_cast<const char*>(tex->pixels.data()), pixel_count);
        if (!out.good()) return false;
      }
    }
  }
  return out.good();
}

bool DeserializeGlTextureLog(std::istream& in, std::vector<GlTextureLogEntry>& out) {
  uint32_t entry_count = 0;
  if (!ReadPod(in, entry_count)) return false;

  std::vector<GlTextureLogEntry> log;
  log.reserve(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    LogEntryKind kind{};
    if (!ReadPod(in, kind)) return false;
    switch (kind) {
      case LogEntryKind::kGenTextures: {
        GenTexturesCall call;
        if (!ReadGLuintVector(in, call.assigned_ids)) return false;
        log.push_back(std::move(call));
        break;
      }
      case LogEntryKind::kDeleteTextures: {
        DeleteTexturesCall call;
        if (!ReadGLuintVector(in, call.ids)) return false;
        log.push_back(std::move(call));
        break;
      }
      case LogEntryKind::kBindTexture: {
        BindTextureCall call{};
        if (!ReadPod(in, call.target)) return false;
        if (!ReadPod(in, call.texture)) return false;
        log.push_back(call);
        break;
      }
      case LogEntryKind::kTexParameter: {
        TexParameterCall call{};
        if (!ReadPod(in, call.target)) return false;
        if (!ReadPod(in, call.pname)) return false;
        if (!ReadPod(in, call.param)) return false;
        log.push_back(call);
        break;
      }
      case LogEntryKind::kTexImage2D: {
        TexImage2DCall call{};
        if (!ReadPod(in, call.target)) return false;
        if (!ReadPod(in, call.level)) return false;
        if (!ReadPod(in, call.internal_format)) return false;
        if (!ReadPod(in, call.width)) return false;
        if (!ReadPod(in, call.height)) return false;
        if (!ReadPod(in, call.format)) return false;
        if (!ReadPod(in, call.type)) return false;
        if (!ReadPod(in, call.has_pixels)) return false;
        uint32_t pixel_count = 0;
        if (!ReadPod(in, pixel_count)) return false;
        call.pixels.resize(pixel_count);
        if (pixel_count != 0) {
          in.read(reinterpret_cast<char*>(call.pixels.data()), pixel_count);
          if (!in.good()) return false;
        }
        log.push_back(std::move(call));
        break;
      }
      default:
        return false;
    }
  }
  out = std::move(log);
  return true;
}

}  // namespace zeebulator
