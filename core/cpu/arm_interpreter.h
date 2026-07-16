#pragma once

#include <array>
#include <stdexcept>

#include "core/cpu/arm_core.h"

namespace zeebulator {

// Thrown for any instruction encoding this v1 interpreter doesn't yet
// implement. Deliberately loud rather than silently mis-executing —
// see TASKS.md Phase 1 for what's covered vs. deferred.
class UnimplementedInstruction : public std::runtime_error {
 public:
  explicit UnimplementedInstruction(const std::string& what)
      : std::runtime_error(what) {}
};

// v1 ARMv6 (ARM1136J-S) interpreter, ARM (A32) state only — no Thumb yet.
// Covers data processing, branch, single word/byte load-store, block
// data transfer (LDM/STM, user-bank/exception-return variants excluded),
// halfword/signed transfer (STRH/LDRH/LDRSB/LDRSH), and BX/BLX
// (branch-and-exchange, register form) as long as the target keeps
// execution in ARM state (bit 0 of the target address clear) — a real
// switch into Thumb state raises UnimplementedInstruction, since Thumb
// decoding isn't implemented at all. Multiply, swap (SWP/SWPB), PSR
// transfer, SWI, and coprocessor instructions also raise
// UnimplementedInstruction.
class ArmInterpreter : public IArmCore {
 public:
  ArmInterpreter();

  void Reset() override;
  void Step() override;
  uint64_t Run(uint64_t max_instructions) override;

  uint32_t GetRegister(int index) const override;
  void SetRegister(int index, uint32_t value) override;

  uint32_t GetCpsr() const override;
  void SetCpsr(uint32_t value) override;

  Memory& GetMemory() override { return memory_; }

  void SetCallOutRange(uint32_t base, uint32_t size) override;
  void SetCallOutHandler(CallOutHandler handler) override;

 private:
  struct Operand2Result {
    uint32_t value;
    bool carry_out;
  };

  bool ConditionPassed(uint32_t cond) const;
  bool GetFlag(CpsrBit bit) const;
  void SetFlag(CpsrBit bit, bool value);

  // Reads a register the way the ISA reads it as an *operand* — R15 reads
  // as (address of the currently-executing instruction) + 8, per the ARM
  // architecture's pipeline-based PC semantics. regs_[kPC] itself always
  // holds the plain, unadjusted address (see .cpp for the invariant this
  // interpreter maintains), so this is the only place the +8 is added.
  uint32_t ReadOperandRegister(uint32_t index) const;

  Operand2Result DecodeOperand2(uint32_t instr) const;

  void ExecuteDataProcessing(uint32_t instr);
  void ExecuteBranch(uint32_t instr);
  void ExecuteSingleDataTransfer(uint32_t instr);
  void ExecuteBlockDataTransfer(uint32_t instr);
  void ExecuteBranchExchange(uint32_t instr);
  void ExecuteHalfwordTransfer(uint32_t instr);

  std::array<uint32_t, 16> regs_{};
  uint32_t cpsr_ = 0;
  Memory memory_;

  // Set by an instruction that explicitly changes control flow (branch,
  // or a data-processing/load writing to R15), so Step() knows not to
  // also apply the default PC += 4.
  bool pc_updated_by_instruction_ = false;

  uint32_t call_out_base_ = 0;
  uint32_t call_out_size_ = 0;
  CallOutHandler call_out_handler_;
};

}  // namespace zeebulator
