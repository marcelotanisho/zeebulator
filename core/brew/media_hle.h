#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/audio/mixer.h"
#include "core/brew/hle_runtime.h"
#include "core/brew/virtual_filesystem.h"
#include "core/loader/midi.h"
#include "core/loader/wav.h"
#include "core/memory/memory.h"

namespace zeebulator {

// IMedia HLE backed by a VirtualFilesystem (itself backed by a loaded
// GGZ archive's contents) and a Mixer. Vtable slot order verified
// directly against real Qualcomm AEEIMedia.h -- see TASKS.md Phase 6:
//   AddRef, Release, QueryInterface, RegisterNotify, SetMediaParm,
//   GetMediaParm, Play, Record, Stop, Seek, Pause, Resume, GetTotalTime,
//   GetState.
//
// Unlike IGL/IEGL, every slot here DOES receive the interface pointer
// ("po") in R0 -- IMedia follows the same convention as IShell/IDisplay/
// IFile, confirmed from the real access macros (e.g. `IMedia_Play(p)` =>
// `AEEGETPVTBL(p, IMedia)->Play(p)`).
//
// Real IMedia exposes almost its entire feature surface through two
// generic slots, SetMediaParm/GetMediaParm(nParamID, p1, p2), rather
// than one vtable slot per feature -- confirmed against real Qualcomm
// sample source (ctsoundmgr.c, AEEMediaUtil.c, research-only). Only the
// params real target-game usage actually needs are implemented:
// MM_PARM_MEDIA_DATA (assign a virtual-filesystem-backed file -- decodes
// it immediately, matching real IMedia's documented "SetMediaData puts
// IMedia in Ready state" behavior), MM_PARM_PLAY_REPEAT (0 = loop
// forever, matching Mixer's boolean loop, anything else = play once --
// exact repeat counts > 1 aren't tracked), and MM_PARM_CHANNEL_SHARE/
// MM_PARM_VOLUME (accepted and stored but not yet applied to playback --
// a real, documented gap, not an oversight).
// Codec is chosen by file extension (.wav -> ParseWav, .mid/.midi ->
// ParseMidi + RenderMidiToPcm -- both produce the same WavAudio shape,
// so everything downstream of decode is codec-agnostic) -- matches what
// the real target game's assets actually are (see TASKS.md Phase 6).
// IMA-ADPCM and MP3 are real BREW codecs but not needed by it, so
// they're not implemented; ParseWav already rejects non-PCM `fmt ` tags
// explicitly rather than mis-decoding them.
//
// RegisterNotify stores the callback but does not fire it yet --
// invoking it on real state transitions (MM_STATUS_START/DONE/ABORT)
// needs a periodic "tick" hook this project doesn't have wired into a
// real run loop yet (Phase 7/8 territory), not a fundamental gap.
class MediaHle {
 public:
  // `object_region_start` is a bump-allocated address range this class
  // owns for newly-created IMedia object headers (4 bytes each) -- must
  // not overlap any other memory region the caller is using.
  MediaHle(Memory& memory, HleRuntime& hle, const VirtualFilesystem& vfs, Mixer& mixer,
           uint32_t object_region_start);

  // Builds the shared IMedia vtable, used by every IMedia instance this
  // class creates.
  void Build(uint32_t vtable_address);

  // Creates a new IMedia object. Mirrors what a real app reaches via
  // ISHELL_CreateInstance(cls, ...) -- this project's established
  // pattern (see IShell/IFile) is for the harness to construct interface
  // objects directly rather than modeling ISHELL_CreateInstance's
  // class-id dispatch, so this is the harness-facing equivalent of that
  // whole real call chain. Returns the IMedia* value the app should
  // receive; Build() must be called first.
  uint32_t CreateMediaObject();

 private:
  enum MediaState {
    kStateIdle = 1,
    kStateReady = 2,
    kStatePlay = 3,
    kStatePlayPause = 5,
  };

  struct Media {
    bool has_data = false;
    int channels = 0;
    int sample_rate = 0;
    std::shared_ptr<const std::vector<int16_t>> samples;
    Mixer::VoiceId voice = 0;
    bool has_voice = false;
    bool loop = false;
    int state = kStateIdle;
    uint32_t notify_fn = 0;
    uint32_t notify_user = 0;
  };

  void RegisterNotifyImpl(IArmCore& core);
  void SetMediaParmImpl(IArmCore& core);
  void GetMediaParmImpl(IArmCore& core);
  void PlayImpl(IArmCore& core);
  void StopImpl(IArmCore& core);
  void PauseImpl(IArmCore& core);
  void ResumeImpl(IArmCore& core);
  void GetTotalTimeImpl(IArmCore& core);
  void GetStateImpl(IArmCore& core);

  uint32_t AllocateMediaObject();

  Memory& memory_;
  HleRuntime& hle_;
  const VirtualFilesystem& vfs_;
  Mixer& mixer_;
  uint32_t vtable_address_ = 0;
  uint32_t next_object_address_;
  std::unordered_map<uint32_t, Media> media_by_object_;
};

}  // namespace zeebulator
