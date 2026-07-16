#include "core/brew/idisplay.h"

#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {
void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }
}  // namespace

IDisplayHle::IDisplayHle(Backend& backend, int width, int height)
    : backend_(backend),
      width_(width),
      height_(height),
      framebuffer_(static_cast<size_t>(width) * height, 0) {}

void IDisplayHle::DrawText(IArmCore& core) {
  // int DrawText(iname* po, AEEFont nFont, const AECHAR* pcText,
  //              int nChars, int x, int y, const AEERect* prcBackground,
  //              uint32 dwFlags)
  // po is R0 (unused here), nFont R1, pcText R2, nChars R3; x/y/rect/flags
  // are the AAPCS stack-passed arguments beyond the first four.
  uint32_t pc_text = core.GetRegister(kR2);
  int32_t n_chars = static_cast<int32_t>(core.GetRegister(kR3));
  int32_t x = static_cast<int32_t>(HleRuntime::ReadStackArg(core, 0));
  int32_t y = static_cast<int32_t>(HleRuntime::ReadStackArg(core, 1));

  // BREW convention: a negative nChars means "null-terminated" -- scan
  // for a zero UTF-16 (AECHAR) code unit.
  int32_t len = n_chars;
  if (len < 0) {
    len = 0;
    while (core.GetMemory().Read16(pc_text + static_cast<uint32_t>(len) * 2) != 0) {
      ++len;
    }
  }

  constexpr int kGlyphW = 6;
  constexpr int kGlyphH = 8;
  for (int cy = 0; cy < kGlyphH; ++cy) {
    for (int cx = 0; cx < len * kGlyphW; ++cx) {
      int px = x + cx;
      int py = y + cy;
      if (px >= 0 && px < width_ && py >= 0 && py < height_) {
        framebuffer_[static_cast<size_t>(py) * width_ + px] = 0xFFFF;  // white
      }
    }
  }
  core.SetRegister(kR0, 0);  // AEE_SUCCESS
}

void IDisplayHle::Update(IArmCore&) {
  backend_.PushVideoFrame(framebuffer_.data(), width_, height_,
                           PixelFormat::kRGB565);
}

uint32_t IDisplayHle::Build(Memory& memory, HleRuntime& hle,
                             uint32_t vtable_address, uint32_t object_address) {
  // Order matches AEEIDisplay.h's INHERIT_IDisplay macro exactly
  // (verified directly against real Qualcomm source -- see TASKS.md
  // Phase 3): AddRef/Release (IBase) through DrawFrame, the pre-BREW-MP
  // slot set (13 total), which is what a 2009-era Zeebo/BREW 4.x
  // IDisplay should have.
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                                    // 0  AddRef
      Stub,                                    // 1  Release
      Stub,                                    // 2  GetFontMetrics
      Stub,                                    // 3  MeasureTextEx
      [this](IArmCore& c) { DrawText(c); },     // 4  DrawText
      Stub,                                    // 5  DrawRect
      Stub,                                    // 6  BitBlt
      [this](IArmCore& c) { Update(c); },       // 7  Update
      Stub,                                    // 8  SetAnnunciators
      Stub,                                    // 9  Backlight
      Stub,                                    // 10 SetColor
      Stub,                                    // 11 GetSymbol
      Stub,                                    // 12 DrawFrame
  };
  return BuildInterfaceObject(memory, hle, vtable_address, object_address,
                               methods);
}

}  // namespace zeebulator
