#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core/backend.h"

namespace zeebulator {

// Sample-accurate software mixer: multiple simultaneously-playing PCM
// voices (short decoded clips -- sound effects, voice lines, or a future
// synthesized MIDI render) summed into one interleaved-stereo buffer and
// pushed to the Backend Abstraction Interface. Mono voices are
// duplicated to both output channels.
//
// Each voice is linearly resampled from its own real decoded rate to
// the Mixer's fixed output rate. The "every real Double Dragon audio
// asset is uniformly 22050Hz" assumption this class used to rely on
// (see TASKS.md Phase 6) turned out to be based on only one of the
// game's real sound channels -- once the other real channels this
// project had been silently failing to create (`0x0100550a`/
// `0x01005501`, PHASE8_LOG.md's "Sound, round twelve") started
// actually decoding real audio, their real declared WAV rates
// differed from 22050Hz, and playing them back frame-for-frame against
// a fixed-rate output (no resampling) is exactly what produced the
// real, confirmed-live "extremely deep" pitch bug that fix surfaced.
//

// Thread-safety: `Mix()` is expected to run on a real-time audio
// callback thread (see Sdl2AudioBackend) while Play/Stop/Pause/Resume
// run on the emulation thread -- guarded by a single mutex, simple
// correctness over throughput since voice counts are always small.
class Mixer {
 public:
  using VoiceId = uint32_t;

  explicit Mixer(int output_sample_rate);

  int OutputSampleRate() const { return output_sample_rate_; }

  // Starts playing `samples` (interleaved if stereo) from the beginning.
  // The Mixer does not decode anything itself -- the caller (IMedia HLE)
  // already produced the whole clip. `loop` repeats from the start
  // instead of finishing. `volume` is 0-100 (AEE_MAX_VOLUME), matching
  // the real IMedia MM_PARM_VOLUME convention this project's own
  // MediaHle already documents -- out-of-range values are clamped, not
  // rejected.
  VoiceId Play(std::shared_ptr<const std::vector<int16_t>> samples, int channels,
               int sample_rate, bool loop, int volume = 100);

  // Changes an already-playing voice's volume in place (real MM_PARM_
  // VOLUME can be set after Play() has already started, not just
  // before it -- see MediaHle::SetMediaParmImpl). No-op for an unknown
  // id.
  void SetVolume(VoiceId id, int volume);

  void Stop(VoiceId id);
  void Pause(VoiceId id);
  void Resume(VoiceId id);
  // False once a non-looping voice has fully played, or after Stop(), or
  // for an unknown id. A paused voice still reports true (it's not
  // finished, just not currently advancing).
  bool IsPlaying(VoiceId id) const;

  // Mixes `frame_count` stereo frames from every active, non-paused
  // voice and pushes the result to `backend` at this Mixer's fixed
  // output rate. Safe to call with zero active voices (pushes silence).
  void Mix(Backend& backend, size_t frame_count);

 private:
  struct Voice {
    VoiceId id;
    std::shared_ptr<const std::vector<int16_t>> samples;
    int channels;
    int sample_rate;
    bool loop;
    int volume = 100;  // 0-100, AEE_MAX_VOLUME convention
    bool paused = false;
    double position_frames = 0.0;  // in source frames, not bytes/samples
    bool finished = false;
  };

  mutable std::mutex mutex_;
  std::vector<Voice> voices_;
  VoiceId next_id_ = 1;
  int output_sample_rate_;
};

}  // namespace zeebulator
