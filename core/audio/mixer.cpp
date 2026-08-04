#include "core/audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace zeebulator {

Mixer::Mixer(int output_sample_rate) : output_sample_rate_(output_sample_rate) {}

Mixer::VoiceId Mixer::Play(std::shared_ptr<const std::vector<int16_t>> samples, int channels,
                            int sample_rate, bool loop, int volume) {
  std::lock_guard<std::mutex> lock(mutex_);
  Voice voice;
  voice.id = next_id_++;
  voice.samples = std::move(samples);
  voice.channels = channels;
  voice.sample_rate = sample_rate;
  voice.loop = loop;
  voice.volume = std::clamp(volume, 0, 100);
  voices_.push_back(std::move(voice));
  return voices_.back().id;
}

void Mixer::SetVolume(VoiceId id, int volume) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Voice& v : voices_) {
    if (v.id == id) v.volume = std::clamp(volume, 0, 100);
  }
}

void Mixer::Stop(VoiceId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                [id](const Voice& v) { return v.id == id; }),
                voices_.end());
}

void Mixer::Pause(VoiceId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Voice& v : voices_) {
    if (v.id == id) v.paused = true;
  }
}

void Mixer::Resume(VoiceId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Voice& v : voices_) {
    if (v.id == id) v.paused = false;
  }
}

bool Mixer::IsPlaying(VoiceId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Voice& v : voices_) {
    if (v.id == id) return !v.finished;
  }
  return false;
}

void Mixer::Mix(Backend& backend, size_t frame_count) {
  std::vector<int32_t> accum(frame_count * 2, 0);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Voice& voice : voices_) {
      if (voice.paused || voice.finished || !voice.samples) continue;
      size_t total_frames = voice.samples->size() / static_cast<size_t>(voice.channels);
      if (total_frames == 0) continue;

      // Linear resampling: real decoded clips don't all share the
      // Mixer's fixed output rate (see this class's own doc comment),
      // so each output frame advances the source position by the
      // voice's own rate ratio rather than 1:1.
      const double step = static_cast<double>(voice.sample_rate) / output_sample_rate_;
      double pos = voice.position_frames;
      for (size_t i = 0; i < frame_count; ++i, pos += step) {
        double sample_pos = pos;
        if (voice.loop) {
          sample_pos = std::fmod(sample_pos, static_cast<double>(total_frames));
        } else if (sample_pos >= static_cast<double>(total_frames)) {
          break;  // this voice has nothing left to contribute this call
        }

        size_t frame0 = static_cast<size_t>(sample_pos);
        size_t frame1 = voice.loop ? (frame0 + 1) % total_frames
                                    : std::min(frame0 + 1, total_frames - 1);
        double frac = sample_pos - static_cast<double>(frame0);

        double gain = voice.volume / 100.0;
        double left, right;
        if (voice.channels == 2) {
          left = ((*voice.samples)[frame0 * 2] * (1.0 - frac) + (*voice.samples)[frame1 * 2] * frac) *
                 gain;
          right = ((*voice.samples)[frame0 * 2 + 1] * (1.0 - frac) +
                    (*voice.samples)[frame1 * 2 + 1] * frac) *
                  gain;
        } else {
          left = right =
              ((*voice.samples)[frame0] * (1.0 - frac) + (*voice.samples)[frame1] * frac) * gain;
        }
        accum[i * 2] += static_cast<int32_t>(left);
        accum[i * 2 + 1] += static_cast<int32_t>(right);
      }

      if (voice.loop) {
        voice.position_frames = std::fmod(pos, static_cast<double>(total_frames));
      } else {
        voice.position_frames = pos;
        if (voice.position_frames >= static_cast<double>(total_frames)) voice.finished = true;
      }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                  [](const Voice& v) { return v.finished; }),
                  voices_.end());
  }

  std::vector<int16_t> out(frame_count * 2);
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<int16_t>(std::clamp<int32_t>(accum[i], -32768, 32767));
  }
  backend.PushAudioSamples(out.data(), frame_count, output_sample_rate_);
}

}  // namespace zeebulator
