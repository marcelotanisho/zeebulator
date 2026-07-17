#pragma once

#include <cstddef>
#include <cstdint>

namespace zeebulator {

// Zeebo's Z-Pad: D-pad, two analog sticks, 7 buttons, ZL/ZR, Home.
// Populated by the frontend (libretro or standalone) on each PollInput call.
struct ZPadState {
  uint16_t buttons = 0;
  int16_t left_stick_x = 0;
  int16_t left_stick_y = 0;
  int16_t right_stick_x = 0;
  int16_t right_stick_y = 0;
};

enum class PixelFormat { kRGB565, kXRGB8888 };

// The seam between the emulation core and the outside world. Both the
// libretro core shim and the standalone frontend implement this; the core
// never knows which one it's talking to. See ARCHITECTURE.md 3.8.
class Backend {
 public:
  virtual ~Backend() = default;

  virtual void PushVideoFrame(const void* framebuffer, int width, int height,
                               PixelFormat format) = 0;
  // `sample_rate` travels with every push rather than being fixed
  // per-Backend or negotiated once: real Double Dragon audio assets are
  // uniformly 22050Hz (confirmed against the actual game data -- see
  // TASKS.md Phase 6), but nothing about the interface should assume
  // every title/asset shares one rate. The Mixer (core/audio/mixer.h)
  // is the only thing that calls this today, and it always passes
  // whatever rate its voices were actually recorded at.
  virtual void PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                                 int sample_rate) = 0;
  virtual ZPadState PollInput() = 0;
};

}  // namespace zeebulator
