#include "core/brew/mod_runtime.h"

#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;
using zeebulator::ModRuntime;

namespace {
constexpr uint32_t kMallocSlotOffset = 0x68;
}  // namespace

TEST(ModRuntime, InstallWritesTablePointerAtModuleBaseMinusFour) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, /*heap_region=*/0x80300000,
                          /*heap_size=*/0x1000);

  mod_runtime.Install(/*module_base=*/0x00100000, /*table_address=*/0x80280000);

  EXPECT_EQ(cpu.GetMemory().Read32(0x00100000 - 4), 0x80280000u);
}

TEST(ModRuntime, MallocSlotReturnsAddressWithinHeapRegion) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, /*heap_region=*/0x80300000,
                          /*heap_size=*/0x1000);
  mod_runtime.Install(/*module_base=*/0x00100000, /*table_address=*/0x80280000);

  uint32_t malloc_fn = cpu.GetMemory().Read32(0x80280000 + kMallocSlotOffset);
  uint32_t result = hle.CallArmFunction(malloc_fn, /*r0=*/36);
  EXPECT_EQ(result, 0x80300000u);
}

TEST(ModRuntime, SuccessiveAllocationsDoNotOverlap) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, /*heap_region=*/0x80300000,
                          /*heap_size=*/0x1000);
  mod_runtime.Install(/*module_base=*/0x00100000, /*table_address=*/0x80280000);
  uint32_t malloc_fn = cpu.GetMemory().Read32(0x80280000 + kMallocSlotOffset);

  uint32_t first = hle.CallArmFunction(malloc_fn, /*r0=*/36);
  uint32_t second = hle.CallArmFunction(malloc_fn, /*r0=*/20);
  EXPECT_EQ(first, 0x80300000u);
  EXPECT_EQ(second, 0x80300000u + 36u);
}

TEST(ModRuntime, AllocationsAreWordAligned) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, /*heap_region=*/0x80300000,
                          /*heap_size=*/0x1000);
  mod_runtime.Install(/*module_base=*/0x00100000, /*table_address=*/0x80280000);
  uint32_t malloc_fn = cpu.GetMemory().Read32(0x80280000 + kMallocSlotOffset);

  uint32_t first = hle.CallArmFunction(malloc_fn, /*r0=*/1);   // rounds up to 4
  uint32_t second = hle.CallArmFunction(malloc_fn, /*r0=*/1);  // should start right after
  EXPECT_EQ(first, 0x80300000u);
  EXPECT_EQ(second, 0x80300000u + 4u);
}

TEST(ModRuntime, ReturnsNullWhenHeapIsExhausted) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, 0xF0000000, 0x1000);
  ModRuntime mod_runtime(cpu.GetMemory(), hle, /*heap_region=*/0x80300000,
                          /*heap_size=*/32);
  mod_runtime.Install(/*module_base=*/0x00100000, /*table_address=*/0x80280000);
  uint32_t malloc_fn = cpu.GetMemory().Read32(0x80280000 + kMallocSlotOffset);

  uint32_t first = hle.CallArmFunction(malloc_fn, /*r0=*/32);
  uint32_t second = hle.CallArmFunction(malloc_fn, /*r0=*/4);
  EXPECT_EQ(first, 0x80300000u);
  EXPECT_EQ(second, 0u);
}
