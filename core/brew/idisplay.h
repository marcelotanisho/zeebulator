#pragma once

#include <cstdint>
#include <vector>

#include "core/backend.h"
#include "core/brew/hle_runtime.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Real IDisplay implementation backing the HLE vtable: owns a simple
// framebuffer and pushes it to the Backend Abstraction Interface
// (ARCHITECTURE.md 3.8) whenever the app calls IDISPLAY_Update. Vtable
// slot order verified directly against Qualcomm's own AEEIDisplay.h --
// see TASKS.md Phase 3.
//
// DrawText does not rasterize real glyphs yet (that's font/graphics
// work, out of scope for M0) -- it draws a simple placeholder block
// sized from the real (x, y, length) arguments, which is enough to prove
// the whole call path (real compiled ARM code -> vtable dispatch -> HLE
// -> framebuffer -> Backend) actually works end to end. It now uses the
// last color SetColor() set instead of a hardcoded white, so text is at
// least visually distinguishable once real games start setting colors
// (confirmed real games do -- see TASKS.md Phase 8).
//
// DrawRect/SetColor treat RGBVAL as the common real-BREW 0x00RRGGBB
// packing (`MAKE_RGB(r,g,b)`, per the real AEEIDisplay.h reference doc
// comment) -- this specific bit layout wasn't independently confirmed
// against a real header this session, unlike the vtable slot order
// itself, which was.
class IDisplayHle {
 public:
  IDisplayHle(Backend& backend, int width, int height);

  // Registers this object's methods with `hle` and writes its vtable +
  // object header into `memory`. Returns the object address (the
  // IDisplay* value the app should receive).
  uint32_t Build(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                 uint32_t object_address);

 private:
  void DrawText(IArmCore& core);
  void DrawRect(IArmCore& core);
  void SetColor(IArmCore& core);
  void Update(IArmCore& core);

  Backend& backend_;
  int width_;
  int height_;
  std::vector<uint16_t> framebuffer_;  // RGB565, width_ * height_
  uint32_t current_rgbval_ = 0x00FFFFFF;  // last color SetColor() set (white by default)
};

}  // namespace zeebulator
