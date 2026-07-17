#include "core/brew/mod_runtime.h"

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;
using zeebulator::ModRuntime;

namespace {
constexpr uint32_t kMemcpySlotOffset = 0x0;
constexpr uint32_t kMemsetSlotOffset = 0x4;
constexpr uint32_t kStrlenSlotOffset = 0x14;
constexpr uint32_t kStrcpySlotOffset = 0x8;
constexpr uint32_t kBoundedStrcpySlotOffset = 0xe4;
constexpr uint32_t kStrstrSlotOffset = 0xe8;
constexpr uint32_t kSprintfSlotOffset = 0x13c;
constexpr uint32_t kMallocSlotOffset = 0x68;
constexpr uint32_t kFreeSlotOffset = 0x6c;
constexpr uint32_t kGetUpTimeMsSlotOffset = 0xb0;
constexpr uint32_t kGetAppContextSlotOffset = 0xc0;
constexpr uint32_t kAppContextShellOffset = 12;
constexpr uint32_t kAppContextDisplayOffset = 20;
constexpr uint32_t kTableAddress = 0x80280000;
constexpr uint32_t kContextAddress = 0x80280200;
constexpr uint32_t kHeapRegion = 0x80300000;
constexpr uint32_t kModuleBase = 0x00100000;
}  // namespace

TEST(ModRuntime, InstallWritesTablePointerAtModuleBaseMinusFour) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);

  mod_runtime.Install(kModuleBase, kTableAddress);

  EXPECT_EQ(cpu.GetMemory().Read32(kModuleBase - 4), kTableAddress);
}

TEST(ModRuntime, MallocSlotReturnsAddressWithinHeapRegion) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);
  uint32_t result = hle.CallArmFunction(malloc_fn, /*r0=*/36);
  EXPECT_EQ(result, kHeapRegion);
}

TEST(ModRuntime, SuccessiveAllocationsDoNotOverlap) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);

  uint32_t first = hle.CallArmFunction(malloc_fn, /*r0=*/36);
  uint32_t second = hle.CallArmFunction(malloc_fn, /*r0=*/20);
  EXPECT_EQ(first, kHeapRegion);
  EXPECT_EQ(second, kHeapRegion + 36u);
}

TEST(ModRuntime, AllocationsAreWordAligned) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);

  uint32_t first = hle.CallArmFunction(malloc_fn, /*r0=*/1);   // rounds up to 4
  uint32_t second = hle.CallArmFunction(malloc_fn, /*r0=*/1);  // should start right after
  EXPECT_EQ(first, kHeapRegion);
  EXPECT_EQ(second, kHeapRegion + 4u);
}

TEST(ModRuntime, ReturnsNullWhenHeapIsExhausted) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/32, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);

  uint32_t first = hle.CallArmFunction(malloc_fn, /*r0=*/32);
  uint32_t second = hle.CallArmFunction(malloc_fn, /*r0=*/4);
  EXPECT_EQ(first, kHeapRegion);
  EXPECT_EQ(second, 0u);
}

TEST(ModRuntime, FreeSlotDoesNotCrash) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t free_fn = cpu.GetMemory().Read32(kTableAddress + kFreeSlotOffset);
  EXPECT_NO_FATAL_FAILURE(hle.CallArmFunction(free_fn, /*ptr=*/kHeapRegion));
}

TEST(ModRuntime, GetAppContextSlotReturnsShellInstanceAtConfirmedOffset) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kShellPtr = 0x80001000;
  mod_runtime.SetShellInstance(kShellPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);
  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(context, kContextAddress);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextShellOffset), kShellPtr);
}

TEST(ModRuntime, GetAppContextSlotReturnsDisplayInstanceAtConfirmedOffset) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kDisplayPtr = 0x80003000;
  mod_runtime.SetDisplayInstance(kDisplayPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);
  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextDisplayOffset), kDisplayPtr);
}

TEST(ModRuntime, SetShellInstanceCanBeCalledAfterInstall) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  constexpr uint32_t kShellPtr = 0x80001000;
  mod_runtime.SetShellInstance(kShellPtr);

  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);
  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextShellOffset), kShellPtr);
}

TEST(ModRuntime, GetUpTimeMsStartsAtZeroAndAdvancesWithTick) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t get_uptime_ms_fn = cpu.GetMemory().Read32(kTableAddress + kGetUpTimeMsSlotOffset);

  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 0u);
  mod_runtime.Tick(16);
  mod_runtime.Tick(16);
  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 32u);
}

TEST(ModRuntime, MemsetFillsExactlyTheRequestedRangeAndReturnsDest) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t memset_fn = cpu.GetMemory().Read32(kTableAddress + kMemsetSlotOffset);

  constexpr uint32_t kDest = 0x80300100;
  cpu.GetMemory().Write8(kDest - 1, 0xAA);  // sentinel just before the range
  cpu.GetMemory().Write8(kDest + 10, 0xAA);  // sentinel just after the range
  // void *memset(void *s, int c, size_t n)
  EXPECT_EQ(hle.CallArmFunction(memset_fn, kDest, /*c=*/0x42, /*n=*/10), kDest);

  for (uint32_t i = 0; i < 10; ++i) {
    EXPECT_EQ(cpu.GetMemory().Read8(kDest + i), 0x42) << "byte " << i;
  }
  EXPECT_EQ(cpu.GetMemory().Read8(kDest - 1), 0xAA) << "wrote before the requested range";
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 10), 0xAA) << "wrote past the requested range";
}

TEST(ModRuntime, StrlenReturnsLengthExcludingNullTerminator) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strlen_fn = cpu.GetMemory().Read32(kTableAddress + kStrlenSlotOffset);

  constexpr uint32_t kStr = 0x80300100;
  const char* text = "hello";
  for (size_t i = 0; text[i] != '\0'; ++i) {
    cpu.GetMemory().Write8(kStr + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  cpu.GetMemory().Write8(kStr + 5, 0);

  EXPECT_EQ(hle.CallArmFunction(strlen_fn, kStr), 5u);
}

TEST(ModRuntime, StrlenReturnsZeroForEmptyString) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strlen_fn = cpu.GetMemory().Read32(kTableAddress + kStrlenSlotOffset);

  constexpr uint32_t kStr = 0x80300100;
  cpu.GetMemory().Write8(kStr, 0);

  EXPECT_EQ(hle.CallArmFunction(strlen_fn, kStr), 0u);
}

TEST(ModRuntime, BoundedStrcpyCopiesUpToRequestedLength) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t bounded_strcpy_fn = cpu.GetMemory().Read32(kTableAddress + kBoundedStrcpySlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  const char* text = "hello";
  for (size_t i = 0; i <= 5; ++i) {
    cpu.GetMemory().Write8(kSrc + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }

  EXPECT_EQ(hle.CallArmFunction(bounded_strcpy_fn, kSrc, /*n=*/6, kDest, /*cap=*/0x200), kDest);
  for (size_t i = 0; i <= 5; ++i) {
    EXPECT_EQ(cpu.GetMemory().Read8(kDest + static_cast<uint32_t>(i)),
              static_cast<uint8_t>(text[i]))
        << "byte " << i;
  }
}

TEST(ModRuntime, BoundedStrcpyNeverExceedsCap) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t bounded_strcpy_fn = cpu.GetMemory().Read32(kTableAddress + kBoundedStrcpySlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  cpu.GetMemory().Write8(kSrc, 0xAB);
  cpu.GetMemory().Write8(kSrc + 1, 0xCD);
  cpu.GetMemory().Write8(kDest + 1, 0x99);  // sentinel: must not be overwritten

  hle.CallArmFunction(bounded_strcpy_fn, kSrc, /*n=*/10, kDest, /*cap=*/1);
  EXPECT_EQ(cpu.GetMemory().Read8(kDest), 0xAB);
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 1), 0x99) << "copied past the cap";
}

TEST(ModRuntime, MemcpyCopiesExactlyTheRequestedRangeAndReturnsDest) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t memcpy_fn = cpu.GetMemory().Read32(kTableAddress + kMemcpySlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  for (uint32_t i = 0; i < 4; ++i) {
    cpu.GetMemory().Write8(kSrc + i, static_cast<uint8_t>(0x10 + i));
  }
  cpu.GetMemory().Write8(kDest - 1, 0xAA);  // sentinel just before the range
  cpu.GetMemory().Write8(kDest + 4, 0xAA);  // sentinel just after the range

  EXPECT_EQ(hle.CallArmFunction(memcpy_fn, kDest, kSrc, /*n=*/4), kDest);
  for (uint32_t i = 0; i < 4; ++i) {
    EXPECT_EQ(cpu.GetMemory().Read8(kDest + i), static_cast<uint8_t>(0x10 + i)) << "byte " << i;
  }
  EXPECT_EQ(cpu.GetMemory().Read8(kDest - 1), 0xAA) << "wrote before the requested range";
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 4), 0xAA) << "wrote past the requested range";
}

TEST(ModRuntime, StrcpyCopiesThroughTheNullTerminatorAndReturnsDest) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strcpy_fn = cpu.GetMemory().Read32(kTableAddress + kStrcpySlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  const char* text = "hi";
  cpu.GetMemory().Write8(kSrc + 0, 'h');
  cpu.GetMemory().Write8(kSrc + 1, 'i');
  cpu.GetMemory().Write8(kSrc + 2, 0);
  cpu.GetMemory().Write8(kDest + 3, 0xAA);  // sentinel just past the null terminator

  EXPECT_EQ(hle.CallArmFunction(strcpy_fn, kDest, kSrc), kDest);
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 0), static_cast<uint8_t>(text[0]));
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 1), static_cast<uint8_t>(text[1]));
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 2), 0u) << "null terminator copied";
  EXPECT_EQ(cpu.GetMemory().Read8(kDest + 3), 0xAAu) << "didn't write past the terminator";
}

namespace {
void WriteCString(zeebulator::Memory& memory, uint32_t addr, const char* text) {
  size_t i = 0;
  for (; text[i] != '\0'; ++i) {
    memory.Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
  }
  memory.Write8(addr + static_cast<uint32_t>(i), 0);
}
}  // namespace

TEST(ModRuntime, StrstrFindsANeedlePresentInTheHaystack) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strstr_fn = cpu.GetMemory().Read32(kTableAddress + kStrstrSlotOffset);

  constexpr uint32_t kHaystack = 0x80300100;
  constexpr uint32_t kNeedle = 0x80300200;
  WriteCString(cpu.GetMemory(), kHaystack, "EGL_ARB_foo EGL_QUALCOMM_COLOR_BUFFER EGL_ARB_bar");
  WriteCString(cpu.GetMemory(), kNeedle, "EGL_QUALCOMM_COLOR_BUFFER");

  EXPECT_EQ(hle.CallArmFunction(strstr_fn, kHaystack, kNeedle), kHaystack + 12);
}

TEST(ModRuntime, StrstrReturnsNullWhenNeedleIsAbsent) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strstr_fn = cpu.GetMemory().Read32(kTableAddress + kStrstrSlotOffset);

  constexpr uint32_t kHaystack = 0x80300100;
  constexpr uint32_t kNeedle = 0x80300200;
  WriteCString(cpu.GetMemory(), kHaystack, "");  // real eglQueryString never returns null, but
                                                  // may return an empty extensions string
  WriteCString(cpu.GetMemory(), kNeedle, "EGL_QUALCOMM_COLOR_BUFFER");

  EXPECT_EQ(hle.CallArmFunction(strstr_fn, kHaystack, kNeedle), 0u);
}

TEST(ModRuntime, StrstrWithEmptyNeedleReturnsHaystack) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t strstr_fn = cpu.GetMemory().Read32(kTableAddress + kStrstrSlotOffset);

  constexpr uint32_t kHaystack = 0x80300100;
  constexpr uint32_t kNeedle = 0x80300200;
  WriteCString(cpu.GetMemory(), kHaystack, "anything");
  WriteCString(cpu.GetMemory(), kNeedle, "");

  EXPECT_EQ(hle.CallArmFunction(strstr_fn, kHaystack, kNeedle), kHaystack);
}

namespace {
std::string ReadCString(zeebulator::Memory& memory, uint32_t addr) {
  std::string s;
  for (uint8_t c = memory.Read8(addr); c != 0; c = memory.Read8(++addr)) {
    s.push_back(static_cast<char>(c));
  }
  return s;
}
}  // namespace

TEST(ModRuntime, SprintfFormatsARealConfirmedErrorCodeMessage) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sprintf_fn = cpu.GetMemory().Read32(kTableAddress + kSprintfSlotOffset);

  constexpr uint32_t kDest = 0x80300100;
  constexpr uint32_t kFmt = 0x80300200;
  constexpr uint32_t kArgs = 0x80300300;
  constexpr uint32_t kArgsCursor = 0x80300400;
  WriteCString(cpu.GetMemory(), kFmt, "ERROR CODE:%d");  // real string, see PHASE8_LOG.md
  cpu.GetMemory().Write32(kArgs, 5);
  cpu.GetMemory().Write32(kArgsCursor, kArgs);

  uint32_t written = hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "ERROR CODE:5");
  EXPECT_EQ(written, 12u);
  EXPECT_EQ(cpu.GetMemory().Read32(kArgsCursor), kArgs + 4) << "cursor advanced past the one arg";
}

TEST(ModRuntime, SprintfSupportsStringHexCharAndLiteralPercent) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sprintf_fn = cpu.GetMemory().Read32(kTableAddress + kSprintfSlotOffset);

  constexpr uint32_t kDest = 0x80300100;
  constexpr uint32_t kFmt = 0x80300200;
  constexpr uint32_t kStr = 0x80300280;
  constexpr uint32_t kArgs = 0x80300300;
  constexpr uint32_t kArgsCursor = 0x80300400;
  WriteCString(cpu.GetMemory(), kFmt, "%s=%x%% [%c]");
  WriteCString(cpu.GetMemory(), kStr, "hp");
  cpu.GetMemory().Write32(kArgs + 0, kStr);
  cpu.GetMemory().Write32(kArgs + 4, 0xFF);
  cpu.GetMemory().Write32(kArgs + 8, static_cast<uint32_t>('!'));
  cpu.GetMemory().Write32(kArgsCursor, kArgs);

  hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "hp=ff% [!]");
}

TEST(ModRuntime, SprintfWithNoDirectivesCopiesTheLiteralTextUnchanged) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sprintf_fn = cpu.GetMemory().Read32(kTableAddress + kSprintfSlotOffset);

  constexpr uint32_t kDest = 0x80300100;
  constexpr uint32_t kFmt = 0x80300200;
  constexpr uint32_t kArgsCursor = 0x80300400;
  WriteCString(cpu.GetMemory(), kFmt, "LOAD ERROR");
  cpu.GetMemory().Write32(kArgsCursor, 0);

  hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "LOAD ERROR");
}
