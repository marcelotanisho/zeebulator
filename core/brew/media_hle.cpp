#include "core/brew/media_hle.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <utility>

namespace zeebulator {

namespace {

void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }
void StubFailed(IArmCore& core) { core.SetRegister(kR0, 1); }  // AEE_EFAILED-ish

// Real MM_PARM_* values from AEEIMedia.h (numeric constants, not
// copyrighted expression -- same rationale as the GLES enum values in
// gl_types.h).
constexpr int kParmVolume = 4;
constexpr int kParmPlayRepeat = 11;
constexpr int kParmMediaData = 1;
constexpr int kParmChannelShare = 16;

constexpr uint32_t kMmdFileName = 0;

std::string ReadCString(Memory& memory, uint32_t addr) {
  std::string s;
  for (uint8_t c = memory.Read8(addr); c != 0; c = memory.Read8(++addr)) {
    s.push_back(static_cast<char>(c));
  }
  return s;
}

bool HasExtension(const std::string& name, const char* ext) {
  size_t ext_len = std::strlen(ext);
  if (name.size() < ext_len) return false;
  return std::equal(name.end() - static_cast<long>(ext_len), name.end(), ext,
                     [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

// Codec is chosen by file extension -- see MediaHle's class doc.
std::optional<WavAudio> DecodeAudioFile(const std::string& name, const std::vector<uint8_t>& data,
                                         int mix_sample_rate) {
  if (HasExtension(name, ".wav")) {
    return ParseWav(data.data(), data.size());
  }
  if (HasExtension(name, ".mid") || HasExtension(name, ".midi")) {
    auto midi = ParseMidi(data.data(), data.size());
    if (!midi) return std::nullopt;
    return RenderMidiToPcm(*midi, mix_sample_rate);
  }
  return std::nullopt;
}

}  // namespace

MediaHle::MediaHle(Memory& memory, HleRuntime& hle, const VirtualFilesystem& vfs, Mixer& mixer,
                    uint32_t object_region_start)
    : memory_(memory), hle_(hle), vfs_(vfs), mixer_(mixer),
      next_object_address_(object_region_start) {}

uint32_t MediaHle::AllocateMediaObject() {
  uint32_t obj_addr = next_object_address_;
  next_object_address_ += 4;
  memory_.Write32(obj_addr, vtable_address_);
  media_by_object_[obj_addr] = Media{};
  return obj_addr;
}

uint32_t MediaHle::CreateMediaObject() { return AllocateMediaObject(); }

void MediaHle::RegisterNotifyImpl(IArmCore& core) {
  // int RegisterNotify(IMedia *po, PFNMEDIANOTIFY pfnNotify, void *pUser)
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, 1);
    return;
  }
  it->second.notify_fn = core.GetRegister(kR1);
  it->second.notify_user = core.GetRegister(kR2);
  core.SetRegister(kR0, 0);
}

void MediaHle::SetMediaParmImpl(IArmCore& core) {
  // int SetMediaParm(IMedia *po, int nParamID, int32 p1, int32 p2)
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, 1);
    return;
  }
  Media& media = it->second;
  auto param_id = static_cast<int32_t>(core.GetRegister(kR1));
  uint32_t p1 = core.GetRegister(kR2);

  if (param_id == kParmMediaData) {
    // p1 -> AEEMediaData { AEECLSID clsData; void *pData; uint32 dwSize; }
    uint32_t cls_data = memory_.Read32(p1 + 0);
    uint32_t data_ptr = memory_.Read32(p1 + 4);
    if (cls_data != kMmdFileName) {
      core.SetRegister(kR0, 1);  // MMD_BUFFER/MMD_ISOURCE not supported yet
      return;
    }
    std::string name = ReadCString(memory_, data_ptr);
    const std::vector<uint8_t>* file_data = vfs_.Find(name);
    if (!file_data) {
      core.SetRegister(kR0, 1);
      return;
    }
    auto decoded = DecodeAudioFile(name, *file_data, mixer_.OutputSampleRate());
    if (!decoded) {
      core.SetRegister(kR0, 1);  // corrupt, or a codec we don't support yet (e.g. IMA-ADPCM/MP3)
      return;
    }
    media.channels = decoded->channels;
    media.sample_rate = decoded->sample_rate;
    media.samples = std::make_shared<const std::vector<int16_t>>(std::move(decoded->samples));
    media.has_data = true;
    media.state = kStateReady;
    core.SetRegister(kR0, 0);
    return;
  }

  if (param_id == kParmPlayRepeat) {
    media.loop = (p1 == 0);  // 0 = forever; exact counts > 1 aren't tracked yet
    core.SetRegister(kR0, 0);
    return;
  }

  if (param_id == kParmChannelShare || param_id == kParmVolume) {
    // Accepted, not yet applied to playback -- see class doc. Returning
    // success (rather than an error) avoids spuriously failing real app
    // logic that doesn't strictly depend on these actually taking effect.
    core.SetRegister(kR0, 0);
    return;
  }

  // Any other parm: no-op success, same reasoning as above.
  core.SetRegister(kR0, 0);
}

void MediaHle::GetMediaParmImpl(IArmCore& core) {
  // int GetMediaParm(IMedia *po, int nParamID, int32 *pP1, int32 *pP2)
  auto param_id = static_cast<int32_t>(core.GetRegister(kR1));
  uint32_t p_p1 = core.GetRegister(kR2);
  if (param_id == kParmVolume) {
    if (p_p1 != 0) memory_.Write32(p_p1, 100);  // AEE_MAX_VOLUME -- see class doc
    core.SetRegister(kR0, 0);
    return;
  }
  core.SetRegister(kR0, 1);  // not implemented for anything else yet
}

void MediaHle::PlayImpl(IArmCore& core) {
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end() || !it->second.has_data) {
    core.SetRegister(kR0, 1);
    return;
  }
  Media& media = it->second;
  if (media.has_voice) mixer_.Stop(media.voice);
  media.voice = mixer_.Play(media.samples, media.channels, media.sample_rate, media.loop);
  media.has_voice = true;
  media.state = kStatePlay;
  core.SetRegister(kR0, 0);
}

void MediaHle::StopImpl(IArmCore& core) {
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, 1);
    return;
  }
  Media& media = it->second;
  if (media.has_voice) {
    mixer_.Stop(media.voice);
    media.has_voice = false;
  }
  media.state = media.has_data ? kStateReady : kStateIdle;
  core.SetRegister(kR0, 0);
}

void MediaHle::PauseImpl(IArmCore& core) {
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end() || !it->second.has_voice) {
    core.SetRegister(kR0, 1);
    return;
  }
  mixer_.Pause(it->second.voice);
  it->second.state = kStatePlayPause;
  core.SetRegister(kR0, 0);
}

void MediaHle::ResumeImpl(IArmCore& core) {
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end() || !it->second.has_voice) {
    core.SetRegister(kR0, 1);
    return;
  }
  mixer_.Resume(it->second.voice);
  it->second.state = kStatePlay;
  core.SetRegister(kR0, 0);
}

void MediaHle::GetTotalTimeImpl(IArmCore& core) {
  // int GetTotalTime(IMedia *po) -- ms, returned directly (no out-param).
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end() || !it->second.has_data || it->second.channels == 0 ||
      it->second.sample_rate == 0) {
    core.SetRegister(kR0, 0);
    return;
  }
  const Media& media = it->second;
  uint64_t frames = media.samples->size() / static_cast<uint32_t>(media.channels);
  uint32_t ms = static_cast<uint32_t>(frames * 1000 / static_cast<uint32_t>(media.sample_rate));
  core.SetRegister(kR0, ms);
}

void MediaHle::GetStateImpl(IArmCore& core) {
  // int GetState(IMedia *po, boolean *pbStateChanging)
  auto it = media_by_object_.find(core.GetRegister(kR0));
  if (it == media_by_object_.end()) {
    core.SetRegister(kR0, kStateIdle);
    return;
  }
  Media& media = it->second;
  if (media.has_voice && !mixer_.IsPlaying(media.voice)) {
    // The voice finished naturally (non-looping playback completed)
    // since we last checked.
    media.has_voice = false;
    media.state = kStateReady;
  }
  uint32_t pb_state_changing = core.GetRegister(kR1);
  if (pb_state_changing != 0) memory_.Write32(pb_state_changing, 0);  // never async in this HLE
  core.SetRegister(kR0, static_cast<uint32_t>(media.state));
}

void MediaHle::Build(uint32_t vtable_address) {
  vtable_address_ = vtable_address;

  // Slot order verified against real AEEIMedia.h -- see class doc.
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                                              // 0  AddRef
      Stub,                                              // 1  Release
      Stub,                                              // 2  QueryInterface
      [this](IArmCore& c) { RegisterNotifyImpl(c); },     // 3  RegisterNotify
      [this](IArmCore& c) { SetMediaParmImpl(c); },       // 4  SetMediaParm
      [this](IArmCore& c) { GetMediaParmImpl(c); },       // 5  GetMediaParm
      [this](IArmCore& c) { PlayImpl(c); },               // 6  Play
      StubFailed,                                        // 7  Record (not implemented)
      [this](IArmCore& c) { StopImpl(c); },               // 8  Stop
      StubFailed,                                        // 9  Seek (not implemented)
      [this](IArmCore& c) { PauseImpl(c); },              // 10 Pause
      [this](IArmCore& c) { ResumeImpl(c); },             // 11 Resume
      [this](IArmCore& c) { GetTotalTimeImpl(c); },       // 12 GetTotalTime
      [this](IArmCore& c) { GetStateImpl(c); },           // 13 GetState
  };
  // Vtable-only: no single "object" here, since a real IMedia object is
  // created per CreateMediaObject() call (this mirrors FileHle's
  // shared-vtable-plus-many-instances pattern).
  for (size_t i = 0; i < methods.size(); ++i) {
    uint32_t sentinel = hle_.Register(methods[i]);
    memory_.Write32(vtable_address + static_cast<uint32_t>(i) * 4, sentinel);
  }
}

}  // namespace zeebulator
