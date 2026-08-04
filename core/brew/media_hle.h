#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/audio/mixer.h"
#include "core/audio/soundfont_synth.h"
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
// MM_PARM_MEDIA_DATA, MM_PARM_PLAY_REPEAT (0 = loop forever, matching
// Mixer's boolean loop, anything else = play once -- exact repeat
// counts > 1 aren't tracked), and MM_PARM_CHANNEL_SHARE/MM_PARM_VOLUME
// (accepted and stored but not yet applied to playback -- a real,
// documented gap, not an oversight).
//
// MM_PARM_MEDIA_DATA supports both real AEEMediaData shapes (TASKS.md/
// PHASE8_LOG.md Phase 8, the sound investigation): MMD_FILE_NAME
// (clsData=0, a virtual-filesystem-backed filename -- decodes
// immediately, matching real IMedia's documented "SetMediaData puts
// IMedia in Ready state" behavior; codec chosen by file extension) and
// MMD_BUFFER (clsData=1, a raw in-memory buffer -- confirmed real and
// necessary, not a guess: Double Dragon's own custom sound.ggz loader
// reads raw bytes directly into a malloc'd buffer rather than through a
// named file, and hands that buffer straight to SetMediaParm; codec
// chosen by sniffing the decoded content's own magic bytes instead of
// an extension, since a raw buffer has none). Real sound.ggz entries
// are gzip-compressed (live-confirmed: a real buffer's first bytes are
// the real gzip magic, with the original filename visible in the
// header's FNAME field) -- gunzipped first when detected, same real
// container convention `core/loader/ggz.cpp` and the runtime-helper
// table's offset-0xdc slot already handle elsewhere in this project.
// MMD_ISOURCE is not implemented (no real evidence this title needs
// it).
// Both MMD_FILE_NAME and MMD_BUFFER converge on the same WavAudio
// decode shape (.wav -> ParseWav, .mid/.midi -> ParseMidi +
// RenderMidiToPcm -- everything downstream of decode is codec-
// agnostic) -- matches what the real target game's assets actually are
// (see TASKS.md Phase 6). IMA-ADPCM and MP3 are real BREW codecs but
// not needed by it, so they're not implemented; ParseWav already
// rejects non-PCM `fmt ` tags explicitly rather than mis-decoding them.
//
// RegisterNotify stores the callback and Tick() fires it (MM_CMD_PLAY,
// MM_STATUS_DONE) once a voice finishes playing naturally -- real
// Double Dragon code depends on this, not just a nice-to-have. Traced
// live (PHASE8_LOG.md, the sound investigation's final round): every
// sound-media object the game creates registers the same real callback
// (`ddragonz.mod` 0x11d020 -> 0x11f4dc), and the real per-attack sound
// system uses a small pool of shared "channels" per character, each
// gated by a stored priority stamp that only a request of equal-or-
// higher priority may reclaim. The DONE notification's own real
// callback body is what resets a channel's stored stamp (or re-arms a
// looping one via Resume) once its sound actually finishes -- without
// ever firing it, a channel a high-priority sound once claimed stays
// permanently unusable by anything lower-priority, which is exactly
// the real, live-reproduced "sound effects stop firing the longer you
// play" symptom this was chasing. The real callback only reads two
// fields of its `AEEMediaCmdNotify`-shaped second argument (confirmed
// via that same disassembly, not guessed): `+8` must equal 4
// (MM_CMD_PLAY) and `+16` is the status (2 = MM_STATUS_DONE, 3 =
// MM_STATUS_ABORT; both route to the same real handler) -- so that's
// all this class populates.
class MediaHle {
 public:
  // `object_region_start` is a bump-allocated address range this class
  // owns for newly-created IMedia object headers (4 bytes each) -- must
  // not overlap any other memory region the caller is using. A small
  // slice near the far end of that same region (see kNotifyScratchOffset)
  // is reserved for the scratch AEEMediaCmdNotify struct Tick() reuses
  // for every real notification it fires, well past any real object
  // count a play session could reach.
  // `soundfont_synth`, if non-null and `IsLoaded()`, is used for real
  // MIDI decode instead of `RenderMidiToPcm`'s hand-rolled synth (see
  // that function's own doc comment) -- optional so existing tests
  // that don't care about real instrument timbre don't have to pay for
  // loading a real ~32MB soundfont just to construct a MediaHle.
  MediaHle(Memory& memory, HleRuntime& hle, const VirtualFilesystem& vfs, Mixer& mixer,
           uint32_t object_region_start, SoundFontSynth* soundfont_synth = nullptr);

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

  // Fires the real MM_STATUS_DONE notification (see class doc) for
  // every voice that has finished playing naturally since the last
  // Tick(), then marks it no longer playing. Call once per real game
  // loop iteration, the same way ModRuntime::Tick()/IShellHle::Tick()
  // already are -- a voice that finishes between two Tick() calls is
  // only noticed at the next one, matching how a real interrupt-driven
  // notification would still need a live app to be pumping its event
  // loop to receive it.
  void Tick();

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
    int volume = 100;  // 0-100, AEE_MAX_VOLUME convention
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

  // Comfortably past any real object count a play session could reach
  // (see kNotifyScratchOffset's own doc comment on the constructor).
  static constexpr uint32_t kNotifyScratchOffset = 0xF0000;

  Memory& memory_;
  HleRuntime& hle_;
  const VirtualFilesystem& vfs_;
  Mixer& mixer_;
  SoundFontSynth* soundfont_synth_;
  uint32_t vtable_address_ = 0;
  uint32_t next_object_address_;
  uint32_t notify_scratch_address_;
  std::unordered_map<uint32_t, Media> media_by_object_;
};

}  // namespace zeebulator
