#include <gtest/gtest.h>

#include "core/brew/hle_runtime.h"
#include "core/brew/scaffold_object.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;

namespace {
constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x1000;
constexpr uint32_t kVtableAddr = 0x80000000;
constexpr uint32_t kObjectAddr = 0x80001000;
}  // namespace

TEST(ScaffoldObject, ObjectHeaderPointsAtVtable) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  uint32_t obj =
      zeebulator::BuildGenericStubObject(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr, 40);

  EXPECT_EQ(obj, kObjectAddr);
  EXPECT_EQ(cpu.GetMemory().Read32(kObjectAddr), kVtableAddr);
}

TEST(ScaffoldObject, EveryRequestedSlotIsCallableAndReturnsZero) {
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  constexpr size_t kSlotCount = 40;
  uint32_t obj = zeebulator::BuildGenericStubObject(cpu.GetMemory(), hle, kVtableAddr, kObjectAddr,
                                                     kSlotCount);

  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    uint32_t sentinel = cpu.GetMemory().Read32(kVtableAddr + slot * 4);
    EXPECT_EQ(hle.CallArmFunction(sentinel, obj, /*arg1=*/0x1234, /*arg2=*/0x5678), 0u)
        << "slot " << slot;
  }
}
