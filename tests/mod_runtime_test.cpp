#include "core/brew/mod_runtime.h"

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;
using zeebulator::ModRuntime;

namespace {
constexpr uint32_t kMemsetSlotOffset = 0x4;
constexpr uint32_t kMallocSlotOffset = 0x68;
constexpr uint32_t kFreeSlotOffset = 0x6c;
constexpr uint32_t kGetUpTimeMsSlotOffset = 0xb0;
constexpr uint32_t kGetAppContextSlotOffset = 0xc0;
constexpr uint32_t kAppContextShellOffset = 12;
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
