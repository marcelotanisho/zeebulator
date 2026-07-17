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
// DrawText rasterizes real glyphs using the small self-authored 5x7
// bitmap font in font5x7.h (uppercase Latin letters, digits, space;
// anything else falls back to a small box) -- lowercase is folded to
// uppercase since the font doesn't have a separate lowercase set. Uses
// the last color SetColor() set instead of a hardcoded white, so text
// reflects real games' actual color choices (confirmed real games set
// one -- see TASKS.md Phase 8).
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

  // The IBitmap* GetDeviceBitmap() should hand back. Real disassembly
  // (TASKS.md Phase 8) shows Double Dragon dereferencing this result's
  // vtable directly, so leaving it unset (0) is fatal -- must be set to
  // a real interface object (see BuildGenericStubObject) before the app
  // can call GetDeviceBitmap without crashing.
  void SetDeviceBitmapInstance(uint32_t bitmap_ptr) { device_bitmap_ptr_ = bitmap_ptr; }

 private:
  void DrawText(IArmCore& core);
  void DrawRect(IArmCore& core);
  void SetColor(IArmCore& core);
  void Update(IArmCore& core);
  void GetDeviceBitmap(IArmCore& core);

  Backend& backend_;
  int width_;
  int height_;
  std::vector<uint16_t> framebuffer_;  // RGB565, width_ * height_
  uint32_t current_rgbval_ = 0x00FFFFFF;  // last color SetColor() set (white by default)
  uint32_t device_bitmap_ptr_ = 0;
};

}  // namespace zeebulator
