#include "frontends/standalone/sdl2_backend.h"

#include <cstdio>

namespace zeebulator {

Sdl2Backend::Sdl2Backend(SDL_Renderer* renderer, int width, int height, int audio_sample_rate)
    : renderer_(renderer),
      texture_(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                  width, height)),
      audio_sample_rate_(audio_sample_rate) {
  SDL_AudioSpec desired{};
  desired.freq = audio_sample_rate;
  desired.format = AUDIO_S16SYS;
  desired.channels = 2;
  desired.samples = 1024;

  SDL_AudioSpec obtained{};
  audio_device_ = SDL_OpenAudioDevice(nullptr, /*iscapture=*/0, &desired, &obtained,
                                       /*allowed_changes=*/0);
  if (audio_device_ == 0) {
    std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    return;
  }
  SDL_PauseAudioDevice(audio_device_, 0);  // start the device unpaused
}

Sdl2Backend::~Sdl2Backend() {
  if (audio_device_ != 0) SDL_CloseAudioDevice(audio_device_);
  SDL_DestroyTexture(texture_);
}

void Sdl2Backend::PushVideoFrame(const void* framebuffer, int width, int height,
                                  PixelFormat format) {
  (void)format;  // IDisplayHle's framebuffer is always RGB565 for now.
  SDL_UpdateTexture(texture_, nullptr, framebuffer, width * 2);
  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
  SDL_RenderPresent(renderer_);
}

void Sdl2Backend::PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                                    int sample_rate) {
  if (audio_device_ == 0 || sample_rate != audio_sample_rate_) return;
  SDL_QueueAudio(audio_device_, interleaved_stereo,
                  static_cast<uint32_t>(frame_count * 2 * sizeof(int16_t)));
}

ZPadState Sdl2Backend::PollInput() { return {}; }

}  // namespace zeebulator
