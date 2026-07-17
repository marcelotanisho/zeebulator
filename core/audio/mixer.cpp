#include "core/audio/mixer.h"

#include <algorithm>
#include <cstdlib>

namespace zeebulator {

Mixer::Mixer(int output_sample_rate) : output_sample_rate_(output_sample_rate) {}

Mixer::VoiceId Mixer::Play(std::shared_ptr<const std::vector<int16_t>> samples, int channels,
                            int sample_rate, bool loop) {
  std::lock_guard<std::mutex> lock(mutex_);
  Voice voice;
  voice.id = next_id_++;
  voice.samples = std::move(samples);
  voice.channels = channels;
  voice.sample_rate = sample_rate;
  voice.loop = loop;
  voices_.push_back(std::move(voice));
  return voices_.back().id;
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

      for (size_t i = 0; i < frame_count; ++i) {
        size_t src_frame = voice.position_frames + i;
        if (voice.loop) {
          src_frame %= total_frames;
        } else if (src_frame >= total_frames) {
          break;  // this voice has nothing left to contribute this call
        }

        int16_t left, right;
        if (voice.channels == 2) {
          left = (*voice.samples)[src_frame * 2];
          right = (*voice.samples)[src_frame * 2 + 1];
        } else {
          left = right = (*voice.samples)[src_frame];
        }
        accum[i * 2] += left;
        accum[i * 2 + 1] += right;
      }

      if (voice.loop) {
        voice.position_frames = (voice.position_frames + frame_count) % total_frames;
      } else {
        voice.position_frames += frame_count;
        if (voice.position_frames >= total_frames) voice.finished = true;
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
