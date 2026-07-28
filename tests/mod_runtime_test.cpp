#include "core/brew/mod_runtime.h"

#include <zlib.h>

#include <vector>

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
constexpr uint32_t kDbgPrintfSlotOffset = 0x9c;
constexpr uint32_t kMemcpyAliasSlotOffset = 0x44;
constexpr uint32_t kReallocSlotOffset = 0x74;
constexpr uint32_t kUnknownSlotOffset0x1b4 = 0x1b4;
constexpr uint32_t kUnknownSlotOffset0xdc = 0xdc;
constexpr uint32_t kAppContextShellOffset = 12;
constexpr uint32_t kAppContextDisplayOffset = 20;
constexpr uint32_t kAppContextThirdObjectOffset = 0x2c;
constexpr uint32_t kAppContextFourthObjectOffset = 0x24;
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

TEST(ModRuntime, ReallocGrowsAndPreservesContent) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);
  uint32_t realloc_fn = cpu.GetMemory().Read32(kTableAddress + kReallocSlotOffset);

  uint32_t original = hle.CallArmFunction(malloc_fn, /*size=*/4);
  cpu.GetMemory().Write32(original, 0xCAFEF00D);

  uint32_t grown = hle.CallArmFunction(realloc_fn, original, /*size=*/16);
  EXPECT_NE(grown, 0u);
  EXPECT_NE(grown, original) << "this allocator never resizes in place";
  EXPECT_EQ(cpu.GetMemory().Read32(grown), 0xCAFEF00Du) << "original content preserved";
}

TEST(ModRuntime, ReallocReturnsNullWithoutTouchingOldBlockWhenHeapIsExhausted) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/8, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t malloc_fn = cpu.GetMemory().Read32(kTableAddress + kMallocSlotOffset);
  uint32_t realloc_fn = cpu.GetMemory().Read32(kTableAddress + kReallocSlotOffset);

  uint32_t original = hle.CallArmFunction(malloc_fn, /*size=*/4);
  cpu.GetMemory().Write32(original, 0x11223344);

  EXPECT_EQ(hle.CallArmFunction(realloc_fn, original, /*size=*/1000), 0u);
  EXPECT_EQ(cpu.GetMemory().Read32(original), 0x11223344u) << "failed realloc leaves the old block alone";
}

TEST(ModRuntime, ReallocWithNullPointerBehavesLikeMalloc) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t realloc_fn = cpu.GetMemory().Read32(kTableAddress + kReallocSlotOffset);

  EXPECT_EQ(hle.CallArmFunction(realloc_fn, /*ptr=*/0, /*size=*/16), kHeapRegion);
}

TEST(ModRuntime, FreeSlotDoesNotCrash) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t free_fn = cpu.GetMemory().Read32(kTableAddress + kFreeSlotOffset);
  EXPECT_NO_FATAL_FAILURE(hle.CallArmFunction(free_fn, /*ptr=*/kHeapRegion));
}

TEST(ModRuntime, UnknownSlot0x1b4DoesNotCrash) {
  // Real disassembly (Double Dragon, TASKS.md Phase 8) shows this slot
  // called as `(dest, count, cap=4, ctor_fn)` -- a shape matching a
  // compiler-generated "construct N array elements" RVCT/EABI helper,
  // but without a visible element-stride argument there's no safe way
  // to implement real construction without guessing (see mod_runtime.h's
  // doc comment) -- registered as a safe no-op instead.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0x1b4);
  EXPECT_NO_FATAL_FAILURE(
      hle.CallArmFunction(fn, /*dest=*/kHeapRegion, /*count=*/4, /*cap=*/4));
}

TEST(ModRuntime, DbgPrintfSlotDoesNotCrash) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t dbgprintf_fn = cpu.GetMemory().Read32(kTableAddress + kDbgPrintfSlotOffset);
  EXPECT_NO_FATAL_FAILURE(hle.CallArmFunction(dbgprintf_fn, /*dest=*/0, /*count=*/4, /*src=*/0));
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

TEST(ModRuntime, GetAppContextSlotReturnsThirdObjectAtConfirmedOffset) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kThirdObjectPtr = 0x80004000;
  mod_runtime.SetThirdContextObject(kThirdObjectPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);
  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextThirdObjectOffset), kThirdObjectPtr);
}

TEST(ModRuntime, GetAppContextSlotReturnsFourthObjectAtConfirmedOffset) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kFourthObjectPtr = 0x80005000;
  mod_runtime.SetFourthContextObject(kFourthObjectPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);

  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);
  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextFourthObjectOffset), kFourthObjectPtr);
}

TEST(ModRuntime, RealCodeWritingAContextFieldDirectlySurvivesASubsequentGetAppContextCall) {
  // Regression test for the bug traced tracing Peggle (TASKS.md Phase
  // 8): real code writes directly into the fourth context field (see
  // the class doc comment), and GetAppContextImpl used to unconditionally
  // rewrite all five fields on every call, silently clobbering that real
  // write the very next time real code called GetAppContext again.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kFourthObjectPtr = 0x80005000;
  mod_runtime.SetFourthContextObject(kFourthObjectPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);

  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  constexpr uint32_t kRealObjectPtr = 0x80300024;
  cpu.GetMemory().Write32(context + kAppContextFourthObjectOffset, kRealObjectPtr);

  hle.CallArmFunction(get_app_context_fn);  // real code's next GetAppContext call
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextFourthObjectOffset), kRealObjectPtr)
      << "a second GetAppContext call must not clobber real code's own direct write";
}

TEST(ModRuntime, SetContextAddressRedirectsGetAppContextToTheNewAddress) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  constexpr uint32_t kShellPtr = 0x80001000;
  mod_runtime.SetShellInstance(kShellPtr);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t get_app_context_fn = cpu.GetMemory().Read32(kTableAddress + kGetAppContextSlotOffset);

  constexpr uint32_t kNewContextAddress = 0x80300024;
  mod_runtime.SetContextAddress(kNewContextAddress);

  uint32_t context = hle.CallArmFunction(get_app_context_fn);
  EXPECT_EQ(context, kNewContextAddress);
  EXPECT_EQ(cpu.GetMemory().Read32(context + kAppContextShellOffset), kShellPtr)
      << "already-set fields must be re-primed onto the new address";
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
  // Each read above also self-advances the clock by 1ms (see
  // GetUpTimeMsImpl's doc comment) -- account for that alongside Tick's
  // own external advances.
  mod_runtime.Tick(16);
  mod_runtime.Tick(16);
  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 33u);
}

TEST(ModRuntime, GetUpTimeMsSelfAdvancesOnEveryReadEvenWithoutTick) {
  // A real busy-wait loop that polls elapsed time entirely within a
  // single native HLE call (no opportunity for the outer per-frame
  // Tick() to run in between -- confirmed by real disassembly, see
  // TASKS.md Phase 8) must still see time pass, or it can never
  // observe its own deadline and would spin forever.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t get_uptime_ms_fn = cpu.GetMemory().Read32(kTableAddress + kGetUpTimeMsSlotOffset);

  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 0u);
  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 1u);
  EXPECT_EQ(hle.CallArmFunction(get_uptime_ms_fn), 2u);
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

TEST(ModRuntime, MemcpyAliasSlotBehavesIdenticallyToMemcpy) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t memcpy_alias_fn = cpu.GetMemory().Read32(kTableAddress + kMemcpyAliasSlotOffset);

  constexpr uint32_t kSrc = 0x80300100;
  constexpr uint32_t kDest = 0x80300200;
  cpu.GetMemory().Write32(kSrc, 0xCAFEF00D);

  EXPECT_EQ(hle.CallArmFunction(memcpy_alias_fn, kDest, kSrc, /*n=*/4), kDest);
  EXPECT_EQ(cpu.GetMemory().Read32(kDest), 0xCAFEF00Du);
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

TEST(ModRuntime, SprintfSupportsARealMinimumFieldWidthAndZeroPadding) {
  // Real Double Dragon HUD text (ddragonz.mod file offset 0x6c0b0,
  // confirmed live via a memory read-watch plus a trap-index count --
  // see PHASE8_LOG.md): "J1 %7d" was rendering completely unsubstituted
  // on screen because this function only ever looked at a single
  // character right after '%' -- the width digit '7' wasn't a
  // recognized directive, so the whole "%7d" fell through to the
  // "unknown directive" fallback and got copied through literally.
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t sprintf_fn = cpu.GetMemory().Read32(kTableAddress + kSprintfSlotOffset);

  constexpr uint32_t kDest = 0x80300100;
  constexpr uint32_t kFmt = 0x80300200;
  constexpr uint32_t kArgs = 0x80300300;
  constexpr uint32_t kArgsCursor = 0x80300400;
  WriteCString(cpu.GetMemory(), kFmt, "J1 %7d");
  cpu.GetMemory().Write32(kArgs, 3);
  cpu.GetMemory().Write32(kArgsCursor, kArgs);

  hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "J1       3");

  // Zero-padding, confirmed distinct from space-padding.
  WriteCString(cpu.GetMemory(), kFmt, "%05d");
  cpu.GetMemory().Write32(kArgs, 3);
  cpu.GetMemory().Write32(kArgsCursor, kArgs);
  hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "00003");

  // A value already at or past the width is never truncated.
  WriteCString(cpu.GetMemory(), kFmt, "%2d");
  cpu.GetMemory().Write32(kArgs, 12345);
  cpu.GetMemory().Write32(kArgsCursor, kArgs);
  hle.CallArmFunction(sprintf_fn, kDest, kFmt, kArgsCursor);
  EXPECT_EQ(ReadCString(cpu.GetMemory(), kDest), "12345");
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

TEST(ModRuntime, DecompressGzipInPlaceSlotDecompressesARealGzipStreamAtTheSameAddress) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t decompress_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0xdc);

  // A real-shaped OBM1 header (core/loader/obm1.h): magic "OI", flag
  // 0x04, bpp 8, width 16, height 8 (both uint16 LE) -- exactly what
  // real code reads via memcpy immediately after this slot returns.
  std::vector<uint8_t> original = {'O', 'I', 0x04, 0x08, 16, 0, 8, 0, 0xAA, 0xBB, 0xCC, 0xDD};
  std::vector<uint8_t> compressed(256);
  z_stream strm{};
  ASSERT_EQ(deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                          Z_DEFAULT_STRATEGY),
            Z_OK);
  strm.next_in = original.data();
  strm.avail_in = static_cast<uInt>(original.size());
  strm.next_out = compressed.data();
  strm.avail_out = static_cast<uInt>(compressed.size());
  ASSERT_EQ(deflate(&strm, Z_FINISH), Z_STREAM_END);
  size_t compressed_size = compressed.size() - strm.avail_out;
  deflateEnd(&strm);

  constexpr uint32_t kBufAddr = 0x80300100;
  for (size_t i = 0; i < compressed_size; ++i) {
    cpu.GetMemory().Write8(kBufAddr + static_cast<uint32_t>(i), compressed[i]);
  }

  EXPECT_EQ(hle.CallArmFunction(decompress_fn, kBufAddr), 0u);
  for (size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(cpu.GetMemory().Read8(kBufAddr + static_cast<uint32_t>(i)), original[i])
        << "byte " << i;
  }
}

TEST(ModRuntime, DecompressGzipInPlaceSlotHandlesInputLargerThanOneChunk) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, kHeapRegion, /*heap_size=*/0x1000, kContextAddress);
  mod_runtime.Install(kModuleBase, kTableAddress);
  uint32_t decompress_fn = cpu.GetMemory().Read32(kTableAddress + kUnknownSlotOffset0xdc);

  // Larger than the implementation's internal 4096-byte streaming
  // chunk size, and incompressible (random-ish, not all-zero) so the
  // real compressed stream is also larger than one chunk -- exercises
  // both the growable-input and growable-output loop paths.
  std::vector<uint8_t> original(10000);
  for (size_t i = 0; i < original.size(); ++i) {
    original[i] = static_cast<uint8_t>((i * 2654435761u) >> 24);
  }
  std::vector<uint8_t> compressed(original.size() + 1024);
  z_stream strm{};
  ASSERT_EQ(deflateInit2(&strm, Z_NO_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY),
            Z_OK);
  strm.next_in = original.data();
  strm.avail_in = static_cast<uInt>(original.size());
  strm.next_out = compressed.data();
  strm.avail_out = static_cast<uInt>(compressed.size());
  ASSERT_EQ(deflate(&strm, Z_FINISH), Z_STREAM_END);
  size_t compressed_size = compressed.size() - strm.avail_out;
  deflateEnd(&strm);
  ASSERT_GT(compressed_size, 4096u) << "test fixture didn't actually exceed one chunk";

  constexpr uint32_t kBufAddr = 0x80300100;
  for (size_t i = 0; i < compressed_size; ++i) {
    cpu.GetMemory().Write8(kBufAddr + static_cast<uint32_t>(i), compressed[i]);
  }

  EXPECT_EQ(hle.CallArmFunction(decompress_fn, kBufAddr), 0u);
  for (size_t i = 0; i < original.size(); ++i) {
    ASSERT_EQ(cpu.GetMemory().Read8(kBufAddr + static_cast<uint32_t>(i)), original[i])
        << "byte " << i;
  }
}
