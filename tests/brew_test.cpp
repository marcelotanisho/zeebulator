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
using zeebulator::IShellHle;
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
  IShellHle shell_hle(cpu.GetMemory(), hle);
  uint32_t shell = shell_hle.Build(kVtableAddr, kObjectAddr);
  EXPECT_EQ(shell, kObjectAddr);

  // AddRef = slot 0, Release = slot 1 -- CreateInstance (slot 2) has real
  // behavior now, tested separately below.
  for (uint32_t slot : {0u, 1u}) {
    uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + slot * 4);
    EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr), 0u);
  }
}

TEST(IShellHle, CreateInstanceReturnsFailedForAnUnregisteredClass) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 2 * 4);
  constexpr uint32_t kPpObjAddr = 0x90000;
  cpu.GetMemory().Write32(kPpObjAddr, 0xDEADBEEF);
  // int CreateInstance(IShell *po, AEECLSID cls, void **ppo)
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, /*cls=*/0x1234, kPpObjAddr), 1u);
}

TEST(IShellHle, CreateInstanceReturnsARegisteredInstance) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  constexpr uint32_t kClsId = 0x01001001;  // AEECLSID_DISPLAY
  constexpr uint32_t kDisplayObj = 0x80003000;
  shell_hle.RegisterInstance(kClsId, kDisplayObj);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + 2 * 4);
  constexpr uint32_t kPpObjAddr = 0x90000;
  EXPECT_EQ(hle.CallArmFunction(sentinel, kObjectAddr, kClsId, kPpObjAddr), 0u);
  EXPECT_EQ(cpu.GetMemory().Read32(kPpObjAddr), kDisplayObj);
}

TEST(IShellHle, SetTimerThenTickFiresAfterElapsedTimeReachesDeadline) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t set_timer = cpu.GetMemory().Read32(kVtableAddr + 11 * 4);
  constexpr uint32_t kCallback = 0x00102000;
  constexpr uint32_t kUserData = 0x80300024;
  // int SetTimer(IShell *ps, uint32 dwCount, PFNNOTIFY pfnNotify, void *pUser)
  EXPECT_EQ(hle.CallArmFunction(set_timer, kObjectAddr, /*dwCount=*/33, kCallback, kUserData), 0u);

  EXPECT_TRUE(shell_hle.Tick(20).empty()) << "shouldn't fire before its deadline";
  auto expired = shell_hle.Tick(13);
  ASSERT_EQ(expired.size(), 1u);
  EXPECT_EQ(expired[0].callback, kCallback);
  EXPECT_EQ(expired[0].user_data, kUserData);
  EXPECT_TRUE(shell_hle.Tick(1000).empty()) << "one-shot timers don't recur on their own";
}

TEST(IShellHle, SetTimerAgainWithSameCallbackReschedulesRatherThanDuplicates) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t set_timer = cpu.GetMemory().Read32(kVtableAddr + 11 * 4);
  constexpr uint32_t kCallback = 0x00102000;
  constexpr uint32_t kUserData = 0x80300024;
  hle.CallArmFunction(set_timer, kObjectAddr, /*dwCount=*/33, kCallback, kUserData);
  hle.CallArmFunction(set_timer, kObjectAddr, /*dwCount=*/33, kCallback, kUserData);

  EXPECT_EQ(shell_hle.Tick(33).size(), 1u) << "re-arming shouldn't create a second pending timer";
}

TEST(IShellHle, CancelTimerRemovesAMatchingPendingTimer) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t set_timer = cpu.GetMemory().Read32(kVtableAddr + 11 * 4);
  uint32_t cancel_timer = cpu.GetMemory().Read32(kVtableAddr + 12 * 4);
  constexpr uint32_t kCallback = 0x00102000;
  constexpr uint32_t kUserData = 0x80300024;
  hle.CallArmFunction(set_timer, kObjectAddr, /*dwCount=*/33, kCallback, kUserData);

  // int CancelTimer(IShell *ps, PFNNOTIFY pfnNotify, void *pUser)
  EXPECT_EQ(hle.CallArmFunction(cancel_timer, kObjectAddr, kCallback, kUserData), 0u);
  EXPECT_TRUE(shell_hle.Tick(1000).empty());
}

TEST(IShellHle, CancelTimerFailsForNoMatchingTimer) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.Build(kVtableAddr, kObjectAddr);

  uint32_t cancel_timer = cpu.GetMemory().Read32(kVtableAddr + 12 * 4);
  EXPECT_EQ(hle.CallArmFunction(cancel_timer, kObjectAddr, /*pfn=*/0x1234, /*pUser=*/0x5678), 1u);
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

  // "HI" is 2 real 5x7 glyphs on 6x8 cells starting at (10,5): H at
  // x=[10,14], I at x=[16,20], with a 1px blank spacing column at x=15.
  auto Px = [&](int x, int y) { return backend.last_frame[static_cast<size_t>(y) * 64 + x]; };
  EXPECT_EQ(Px(10, 5), 0xFFFFu) << "H top-left corner is set (H's top row is #...#)";
  EXPECT_EQ(Px(12, 5), 0u) << "H top-middle is clear (H's top row is #...#)";
  EXPECT_EQ(Px(10, 8), 0xFFFFu) << "H's crossbar row is set across the glyph";
  EXPECT_EQ(Px(12, 8), 0xFFFFu) << "H's crossbar row is set across the glyph";
  EXPECT_EQ(Px(15, 5), 0u) << "1px spacing column between glyphs stays clear";
  EXPECT_EQ(Px(16, 5), 0xFFFFu) << "I's top row is full (#####)";
  EXPECT_EQ(Px(18, 8), 0xFFFFu) << "I's centered stem is set on the crossbar row";
  EXPECT_EQ(Px(16, 8), 0u) << "I's stem is centered, not on the left edge";
  EXPECT_EQ(backend.last_frame[0], 0u) << "untouched pixel stays black";
}

TEST(IDisplayHle, DrawRectWithNullRectFillsWholeScreen) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 4, 3);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  // void DrawRect(iname *po, const AEERect *pRect, RGBVAL clrFrame, RGBVAL clrFill, uint32 dwFlags)
  uint32_t draw_rect_sentinel = cpu.GetMemory().Read32(kVtableAddr + 5 * 4);
  hle.CallArmFunction(draw_rect_sentinel, display_obj, /*pRect=*/0, /*clrFrame=*/0,
                       /*clrFill=*/0x00FF0000);  // red

  uint32_t update_sentinel = cpu.GetMemory().Read32(kVtableAddr + 7 * 4);
  hle.CallArmFunction(update_sentinel, display_obj);

  ASSERT_EQ(backend.push_count, 1);
  for (int i = 0; i < 4 * 3; ++i) {
    EXPECT_EQ(backend.last_frame[static_cast<size_t>(i)], 0xF800u) << "pixel " << i;  // RGB565 red
  }
}

TEST(IDisplayHle, DrawRectWithExplicitRectFillsOnlyThatArea) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 8, 8);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  // Real AEERect: { int16 x, y, dx, dy; }
  constexpr uint32_t kRectAddr = 0x9000;
  cpu.GetMemory().Write16(kRectAddr + 0, 2);  // x
  cpu.GetMemory().Write16(kRectAddr + 2, 1);  // y
  cpu.GetMemory().Write16(kRectAddr + 4, 3);  // dx
  cpu.GetMemory().Write16(kRectAddr + 6, 2);  // dy

  uint32_t draw_rect_sentinel = cpu.GetMemory().Read32(kVtableAddr + 5 * 4);
  hle.CallArmFunction(draw_rect_sentinel, display_obj, kRectAddr, /*clrFrame=*/0,
                       /*clrFill=*/0x0000FF00);  // green

  uint32_t update_sentinel = cpu.GetMemory().Read32(kVtableAddr + 7 * 4);
  hle.CallArmFunction(update_sentinel, display_obj);

  ASSERT_EQ(backend.push_count, 1);
  EXPECT_EQ(backend.last_frame[1 * 8 + 2], 0x07E0u) << "inside the rect";  // RGB565 green
  EXPECT_EQ(backend.last_frame[1 * 8 + 4], 0x07E0u) << "still inside (x=4 < 2+3)";
  EXPECT_EQ(backend.last_frame[1 * 8 + 5], 0u) << "outside the rect (x=5 >= 2+3)";
  EXPECT_EQ(backend.last_frame[0], 0u) << "untouched pixel stays black";
}

TEST(IDisplayHle, SetColorChangesDrawTextColorAndReturnsPrevious) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  TestBackend backend;
  IDisplayHle display(backend, 64, 48);
  uint32_t display_obj = display.Build(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr);

  uint32_t set_color_sentinel = cpu.GetMemory().Read32(kVtableAddr + 10 * 4);
  // RGBVAL SetColor(iname *po, AEEClrItem clr, RGBVAL rgb)
  uint32_t previous =
      hle.CallArmFunction(set_color_sentinel, display_obj, /*clr=*/0, /*rgb=*/0x000000FF);  // blue
  EXPECT_EQ(previous, 0x00FFFFFFu) << "default color is white before any SetColor call";

  WriteUtf16String(cpu.GetMemory(), 0x3000, "H");
  cpu.SetRegister(zeebulator::kSP, 0x9000);
  cpu.GetMemory().Write32(0x9000, 0);  // x
  cpu.GetMemory().Write32(0x9004, 0);  // y
  cpu.GetMemory().Write32(0x9008, 0);  // prcBackground
  cpu.GetMemory().Write32(0x900C, 0);  // dwFlags
  uint32_t draw_text_sentinel = cpu.GetMemory().Read32(kVtableAddr + 4 * 4);
  hle.CallArmFunction(draw_text_sentinel, display_obj, /*nFont=*/0, /*pcText=*/0x3000,
                       /*nChars=*/static_cast<uint32_t>(-1));

  uint32_t update_sentinel = cpu.GetMemory().Read32(kVtableAddr + 7 * 4);
  hle.CallArmFunction(update_sentinel, display_obj);

  ASSERT_EQ(backend.push_count, 1);
  EXPECT_EQ(backend.last_frame[0], 0x001Fu) << "drawn glyph uses the newly-set blue color";
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
