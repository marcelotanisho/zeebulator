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
// one -- see PHASE8_LOG.md).
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
  // (PHASE8_LOG.md) shows Double Dragon dereferencing this result's
  // vtable directly, so leaving it unset (0) is fatal -- must be set to
  // a real interface object (see BuildGenericStubObject) before the app
  // can call GetDeviceBitmap without crashing.
  void SetDeviceBitmapInstance(uint32_t bitmap_ptr) { device_bitmap_ptr_ = bitmap_ptr; }

  // Re-pushes the *last frame real app code actually committed via
  // IDISPLAY_Update* to the backend -- a no-op if Update() has never
  // been called. Meant to be called continuously by the frontend, at
  // least roughly once a second, for as long as the app runs -- found
  // necessary tracing Double Dragon (TASKS.md Phase 8), confirmed
  // directly against a real desktop (not just automated screenshots):
  // real code can legitimately call Update() exactly once (a static
  // screen that never redraws) and real content is correctly computed
  // and pushed, but at least one real desktop environment's window
  // compositor stops actually displaying a window's content within
  // about a second of it no longer receiving freshly presented frames,
  // even though every real present succeeded and stayed byte-for-byte
  // identical. Real display hardware has no such requirement (whatever
  // was last drawn simply stays lit), so this compensates for that one
  // specific environment's behavior rather than being a real BREW/Zeebo
  // behavior in its own right -- and it needs to keep happening for the
  // app's entire lifetime, not just once after the first real frame:
  // a single priming burst right after the first Update() was tried
  // and confirmed (directly, on a real desktop) to only postpone the
  // blackout by exactly as long as the burst itself lasted.
  //
  // Deliberately re-presents a snapshot taken *at the real Update()
  // call*, not the live `framebuffer_` directly -- real code can call
  // DrawRect/DrawText again later without a following Update() (e.g.
  // mid-way through composing the next real frame), and presenting that
  // live, not-yet-committed content would show something real app code
  // never actually intended to be visible.
  void RepresentLastFrame() {
    if (!has_presented_) return;
    backend_.PushVideoFrame(last_presented_.data(), width_, height_, PixelFormat::kRGB565);
  }

  // Alpha-composites a real RGBA source image (`w`*`h`*4 bytes,
  // row-major, straight alpha) onto the framebuffer at (`x`, `y`),
  // clipped to the display bounds. Not part of the real BREW IDisplay
  // ABI -- real IDISPLAY_BitBlt takes a real IBitmap source object, not
  // raw pixels -- this exists for titles that draw entirely through
  // their own in-app rendering engine and never call any real IDisplay
  // method at all (Alien Breaker Deluxe: TASKS.md Phase 8, the real
  // font-atlas glyph bridge), so there's no real ARM call site for a
  // real BitBlt implementation to intercept; the bridge has to
  // originate from this project's own emulator-side code instead.
  // Alpha is treated as fully-transparent-or-fully-opaque (no partial
  // blending) -- correct for this session's own confirmed real font
  // atlas (a clean-edged bitmap font, not anti-aliased), not a general
  // real alpha-blend implementation.
  void BlitRgba(int x, int y, int w, int h, const uint8_t* rgba) {
    for (int row = 0; row < h; ++row) {
      int py = y + row;
      if (py < 0 || py >= height_) continue;
      for (int col = 0; col < w; ++col) {
        int px = x + col;
        if (px < 0 || px >= width_) continue;
        const uint8_t* texel = rgba + (static_cast<size_t>(row) * w + col) * 4;
        if (texel[3] == 0) continue;
        uint16_t color = static_cast<uint16_t>(((texel[0] >> 3) << 11) | ((texel[1] >> 2) << 5) | (texel[2] >> 3));
        framebuffer_[static_cast<size_t>(py) * width_ + px] = color;
      }
    }
  }

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
  std::vector<uint16_t> last_presented_;  // snapshot as of the last real Update() call
  bool has_presented_ = false;
  uint32_t current_rgbval_ = 0x00FFFFFF;  // last color SetColor() set (white by default)
  uint32_t device_bitmap_ptr_ = 0;
};

}  // namespace zeebulator
