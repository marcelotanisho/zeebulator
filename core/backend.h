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
  virtual void PushAudioSamples(const int16_t* interleaved_stereo,
                                 size_t frame_count) = 0;
  virtual ZPadState PollInput() = 0;
};

}  // namespace zeebulator
