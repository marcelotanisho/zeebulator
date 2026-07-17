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
// Deliberately does not resample: every voice is read frame-for-frame
// against the Mixer's fixed output rate. A voice recorded at a different
// rate would play back at the wrong pitch/speed -- a real, documented
// limitation, not currently a problem since every real Double Dragon
// audio asset is uniformly 22050Hz (confirmed against the actual game
// data, see TASKS.md Phase 6). Revisit if a future title needs mixed
// sample rates.
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
  // instead of finishing.
  VoiceId Play(std::shared_ptr<const std::vector<int16_t>> samples, int channels,
               int sample_rate, bool loop);

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
    bool paused = false;
    size_t position_frames = 0;  // in source frames, not bytes/samples
    bool finished = false;
  };

  mutable std::mutex mutex_;
  std::vector<Voice> voices_;
  VoiceId next_id_ = 1;
  int output_sample_rate_;
};

}  // namespace zeebulator
