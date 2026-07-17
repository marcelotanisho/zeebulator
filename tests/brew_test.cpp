#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/brew/idisplay.h"
#include "core/brew/ishell.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::Backend;
using zeebulator::HleRuntime;
using zeebulator::IDisplayHle;
using zeebulator::PixelFormat;
using zeebulator::ZPadState;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x1000;
constexpr uint32_t kVtableAddr = 0x80000000;
constexpr uint32_t kObjectAddr = 0x80001000;

class TestBackend : public Backend {
 public:
  void PushVideoFrame(const void* framebuffer, int width, int height,
                       PixelFormat format) override {
    push_count++;
    last_width = width;
    last_height = height;
    last_format = format;
    last_frame.assign(static_cast<const uint16_t*>(framebuffer),
                       static_cast<const uint16_t*>(framebuffer) +
                           static_cast<size_t>(width) * height);
  }
  void PushAudioSamples(const int16_t*, size_t, int) override {}
  ZPadState PollInput() override { return {}; }

  int push_count = 0;
  int last_width = 0;
  int last_height = 0;
  PixelFormat last_format = PixelFormat::kRGB565;
  std::vector<uint16_t> last_frame;
};

void WriteUtf16String(zeebulator::Memory& mem, uint32_t addr,
                       const std::string& text) {
  for (size_t i = 0; i < text.size(); ++i) {
    mem.Write16(addr + static_cast<uint32_t>(i) * 2,
                static_cast<uint16_t>(text[i]));
  }
  mem.Write16(addr + static_cast<uint32_t>(text.size()) * 2, 0);
}

}  // namespace

TEST(IShellHle, AllStubbedMethodsReturnZeroWithoutCrashing) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  uint32_t shell = zeebulator::BuildIShell(cpu.GetMemory(), hle, kVtableAddr,
                                            kObjectAddr);
  EXPECT_EQ(shell, kObjectAddr);

  // AddRef = slot 0, Release = slot 1, CreateInstance = slot 2.
  for (uint32_t slot : {0u, 1u, 2u}) {
    uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + slot * 4);
    EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr), 0u);
  }
}

TEST(IDisplayHle, DrawTextThenUpdatePushesCorrectFrame) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t display_obj =
      display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  WriteUtf16String(cpu.GetMemory(), 0x3000, "HI");

  // Stack args beyond R0-R3: x, y, prcBackground, dwFlags.
  cpu.SetRegister(zeebulator::kSP, 0x9000);
  cpu.GetMemory().Write32(0x9000, 10);  // x
  cpu.GetMemory().Write32(0x9004, 5);   // y
  cpu.GetMemory().Write32(0x9008, 0);   // prcBackground
  cpu.GetMemory().Write32(0x900C, 0);   // dwFlags

  uint32_t draw_text_sentinel = cpu.GetMemory().Read32(kVtableAddr + 4 * 4);
  hle.CallArmFunction(draw_text_sentinel, display_obj, /*nFont=*/0,
                       /*pcText=*/0x3000, /*nChars=*/static_cast<uint32_t>(-1));

  EXPECT_EQ(backend.push_count, 0) << "DrawText alone shouldn't push a frame";

  uint32_t update_sentinel = cpu.GetMemory().Read32(kVtableAddr + 7 * 4);
  hle.CallArmFunction(update_sentinel, display_obj);

  ASSERT_EQ(backend.push_count, 1);
  EXPECT_EQ(backend.last_width, 64);
  EXPECT_EQ(backend.last_height, 48);
  EXPECT_EQ(backend.last_format, PixelFormat::kRGB565);

  // "HI" is 2 chars, glyph block is 6x8 -> a 12x8 white block at (10,5).
  EXPECT_EQ(backend.last_frame[5 * 64 + 10], 0xFFFF) << "inside the drawn block";
  EXPECT_EQ(backend.last_frame[5 * 64 + 21], 0xFFFF) << "still inside (x=21 < 10+12)";
  EXPECT_EQ(backend.last_frame[5 * 64 + 23], 0u) << "outside the drawn block (x=23 >= 10+12)";
  EXPECT_EQ(backend.last_frame[0], 0u) << "untouched pixel stays black";
}

TEST(IDisplayHle, ObjectAddressPointsAtVtable) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  EXPECT_EQ(obj, kObjectAddr);
  EXPECT_EQ(cpu.GetMemory().Read32(kObjectAddr), kVtableAddr)
      << "object header's first word must point at the vtable";
}
