#include "core/cpu/arm_interpreter.h"

namespace zeebulator {

namespace {

uint32_t RotateRight(uint32_t value, uint32_t amount) {
  amount &= 31;
  return amount == 0 ? value : (value >> amount) | (value << (32 - amount));
}

// Shared add/subtract-with-flags core: every data-processing arithmetic
// opcode (SUB/RSB/ADD/ADC/SBC/RSC/CMP/CMN) reduces to this by choosing
// operands and carry_in appropriately (SUB(a,b) = Add(a, ~b, 1), etc.)
uint32_t AddWithFlags(uint32_t a, uint32_t b, uint32_t carry_in,
                       bool& carry_out, bool& overflow_out) {
  uint64_t sum = static_cast<uint64_t>(a) + static_cast<uint64_t>(b) + carry_in;
  uint32_t result = static_cast<uint32_t>(sum);
  carry_out = (sum >> 32) != 0;
  overflow_out = ((~(a ^ b) & (a ^ result)) >> 31) & 1;
  return result;
}

// ARM shifter, used for both operand2 register shifts and LDR/STR
// register-offset shifts. `is_immediate_shift` selects between the
// immediate-shift-amount special cases (shift #0 has type-specific
// meaning, e.g. ROR #0 == RRX) and the register-shift-amount ones
// (amount 0 always means "no shift, carry unchanged").
uint32_t ShiftWithCarry(uint32_t value, uint32_t shift_type,
                         uint32_t shift_amount, bool carry_in,
                         bool& carry_out, bool is_immediate_shift) {
  switch (shift_type) {
    case 0:  // LSL
      if (shift_amount == 0) {
        carry_out = carry_in;
        return value;
      }
      if (shift_amount >= 32) {
        carry_out = (shift_amount == 32) ? (value & 1) : false;
        return 0;
      }
      carry_out = (value >> (32 - shift_amount)) & 1;
      return value << shift_amount;

    case 1:  // LSR
      if (shift_amount == 0) {
        if (is_immediate_shift) {  // LSR #0 encodes LSR #32
          carry_out = (value >> 31) & 1;
          return 0;
        }
        carry_out = carry_in;
        return value;
      }
      if (shift_amount >= 32) {
        carry_out = (shift_amount == 32) ? ((value >> 31) & 1) : false;
        return 0;
      }
      carry_out = (value >> (shift_amount - 1)) & 1;
      return value >> shift_amount;

    case 2: {  // ASR
      if (shift_amount == 0) {
        if (is_immediate_shift) {  // ASR #0 encodes ASR #32
          carry_out = (value >> 31) & 1;
          return carry_out ? 0xFFFFFFFFu : 0u;
        }
        carry_out = carry_in;
        return value;
      }
      if (shift_amount >= 32) {
        carry_out = (value >> 31) & 1;
        return carry_out ? 0xFFFFFFFFu : 0u;
      }
      carry_out = (value >> (shift_amount - 1)) & 1;
      return static_cast<uint32_t>(static_cast<int32_t>(value) >> shift_amount);
    }

    case 3:  // ROR / RRX
      if (shift_amount == 0) {
        if (is_immediate_shift) {  // ROR #0 encodes RRX
          carry_out = value & 1;
          return (static_cast<uint32_t>(carry_in) << 31) | (value >> 1);
        }
        carry_out = carry_in;
        return value;
      }
      if ((shift_amount & 31) == 0) {
        carry_out = (value >> 31) & 1;
        return value;
      }
      carry_out = (value >> ((shift_amount & 31) - 1)) & 1;
      return RotateRight(value, shift_amount);
  }
  carry_out = carry_in;
  return value;
}

}  // namespace

ArmInterpreter::ArmInterpreter() { Reset(); }

void ArmInterpreter::Reset() {
  regs_.fill(0);
  cpsr_ = 0;
}

bool ArmInterpreter::GetFlag(CpsrBit bit) const { return (cpsr_ >> bit) & 1; }

void ArmInterpreter::SetFlag(CpsrBit bit, bool value) {
  if (value) {
    cpsr_ |= (1u << bit);
  } else {
    cpsr_ &= ~(1u << bit);
  }
}

uint32_t ArmInterpreter::GetRegister(int index) const { return regs_[index]; }

void ArmInterpreter::SetRegister(int index, uint32_t value) {
  regs_[index] = value;
}

uint32_t ArmInterpreter::GetCpsr() const { return cpsr_; }
void ArmInterpreter::SetCpsr(uint32_t value) { cpsr_ = value; }

void ArmInterpreter::SetCallOutRange(uint32_t base, uint32_t size) {
  call_out_base_ = base;
  call_out_size_ = size;
}

void ArmInterpreter::SetCallOutHandler(CallOutHandler handler) {
  call_out_handler_ = std::move(handler);
}

uint32_t ArmInterpreter::ReadOperandRegister(uint32_t index) const {
  // regs_[kPC] always holds the address of the instruction currently
  // being fetched/executed (see Step()); the ISA defines a register-read
  // of R15 as that address + 8, so the adjustment happens only here.
  return index == kPC ? regs_[kPC] + 8 : regs_[index];
}

bool ArmInterpreter::ConditionPassed(uint32_t cond) const {
  bool n = GetFlag(kCpsrN), z = GetFlag(kCpsrZ), c = GetFlag(kCpsrC),
       v = GetFlag(kCpsrV);
  switch (cond) {
    case 0x0: return z;                    // EQ
    case 0x1: return !z;                   // NE
    case 0x2: return c;                    // CS/HS
    case 0x3: return !c;                   // CC/LO
    case 0x4: return n;                    // MI
    case 0x5: return !n;                   // PL
    case 0x6: return v;                    // VS
    case 0x7: return !v;                   // VC
    case 0x8: return c && !z;              // HI
    case 0x9: return !c || z;              // LS
    case 0xA: return n == v;               // GE
    case 0xB: return n != v;               // LT
    case 0xC: return !z && (n == v);       // GT
    case 0xD: return z || (n != v);        // LE
    case 0xE: return true;                 // AL
    default: return false;                 // NV (reserved) — v1: never
  }
}

ArmInterpreter::Operand2Result ArmInterpreter::DecodeOperand2(
    uint32_t instr) const {
  bool current_carry = GetFlag(kCpsrC);
  if ((instr >> 25) & 1) {
    uint32_t rotate_imm = (instr >> 8) & 0xF;
    uint32_t imm8 = instr & 0xFF;
    uint32_t value = RotateRight(imm8, rotate_imm * 2);
    bool carry_out = (rotate_imm == 0) ? current_carry : ((value >> 31) & 1);
    return {value, carry_out};
  }

  uint32_t rm = instr & 0xF;
  uint32_t rm_val = ReadOperandRegister(rm);
  uint32_t shift_type = (instr >> 5) & 0x3;
  bool is_immediate_shift = ((instr >> 4) & 1) == 0;
  uint32_t shift_amount;
  if (is_immediate_shift) {
    shift_amount = (instr >> 7) & 0x1F;
  } else {
    uint32_t rs = (instr >> 8) & 0xF;
    shift_amount = ReadOperandRegister(rs) & 0xFF;
  }
  bool carry_out;
  uint32_t value = ShiftWithCarry(rm_val, shift_type, shift_amount,
                                   current_carry, carry_out, is_immediate_shift);
  return {value, carry_out};
}

void ArmInterpreter::ExecuteDataProcessing(uint32_t instr) {
  uint32_t opcode = (instr >> 21) & 0xF;
  bool s = ((instr >> 20) & 1) != 0;
  uint32_t rn = (instr >> 16) & 0xF;
  uint32_t rd = (instr >> 12) & 0xF;

  Operand2Result op2 = DecodeOperand2(instr);
  uint32_t rn_val = ReadOperandRegister(rn);

  uint32_t result = 0;
  bool write_result = true;
  bool carry_out = op2.carry_out;
  bool overflow_out = GetFlag(kCpsrV);

  switch (opcode) {
    case 0x0: result = rn_val & op2.value; break;                        // AND
    case 0x1: result = rn_val ^ op2.value; break;                        // EOR
    case 0x2: result = AddWithFlags(rn_val, ~op2.value, 1, carry_out, overflow_out); break;  // SUB
    case 0x3: result = AddWithFlags(op2.value, ~rn_val, 1, carry_out, overflow_out); break;  // RSB
    case 0x4: result = AddWithFlags(rn_val, op2.value, 0, carry_out, overflow_out); break;   // ADD
    case 0x5: result = AddWithFlags(rn_val, op2.value, GetFlag(kCpsrC), carry_out, overflow_out); break;  // ADC
    case 0x6: result = AddWithFlags(rn_val, ~op2.value, GetFlag(kCpsrC), carry_out, overflow_out); break; // SBC
    case 0x7: result = AddWithFlags(op2.value, ~rn_val, GetFlag(kCpsrC), carry_out, overflow_out); break; // RSC
    case 0x8: result = rn_val & op2.value; write_result = false; break;  // TST
    case 0x9: result = rn_val ^ op2.value; write_result = false; break;  // TEQ
    case 0xA: result = AddWithFlags(rn_val, ~op2.value, 1, carry_out, overflow_out); write_result = false; break;  // CMP
    case 0xB: result = AddWithFlags(rn_val, op2.value, 0, carry_out, overflow_out); write_result = false; break;   // CMN
    case 0xC: result = rn_val | op2.value; break;                        // ORR
    case 0xD: result = op2.value; break;                                 // MOV
    case 0xE: result = rn_val & ~op2.value; break;                       // BIC
    case 0xF: result = ~op2.value; break;                                // MVN
  }

  if (write_result) {
    if (rd == kPC) {
      regs_[kPC] = result;
      pc_updated_by_instruction_ = true;
    } else {
      regs_[rd] = result;
    }
  }

  if (s) {
    if (rd == kPC && write_result) {
      throw UnimplementedInstruction(
          "S=1 with Rd=R15 (SPSR restore) not supported");
    }
    SetFlag(kCpsrN, (result >> 31) & 1);
    SetFlag(kCpsrZ, result == 0);
    SetFlag(kCpsrC, carry_out);
    SetFlag(kCpsrV, overflow_out);
  }
}

void ArmInterpreter::ExecuteBranch(uint32_t instr) {
  bool link = (instr >> 24) & 1;
  int32_t offset = static_cast<int32_t>(instr << 8) >> 6;
  uint32_t fetch_addr = regs_[kPC];
  if (link) {
    regs_[kLR] = fetch_addr + 4;
  }
  regs_[kPC] = static_cast<uint32_t>(static_cast<int32_t>(fetch_addr) + 8 + offset);
  pc_updated_by_instruction_ = true;
}

void ArmInterpreter::ExecuteSingleDataTransfer(uint32_t instr) {
  bool immediate_offset = ((instr >> 25) & 1) == 0;
  bool pre_indexed = (instr >> 24) & 1;
  bool add_offset = (instr >> 23) & 1;
  bool byte_transfer = (instr >> 22) & 1;
  bool write_back = (instr >> 21) & 1;
  bool load = (instr >> 20) & 1;
  uint32_t rn = (instr >> 16) & 0xF;
  uint32_t rd = (instr >> 12) & 0xF;

  uint32_t offset;
  if (immediate_offset) {
    offset = instr & 0xFFF;
  } else {
    uint32_t rm = instr & 0xF;
    uint32_t rm_val = ReadOperandRegister(rm);
    uint32_t shift_type = (instr >> 5) & 0x3;
    uint32_t shift_amount = (instr >> 7) & 0x1F;
    bool carry_out_unused;
    offset = ShiftWithCarry(rm_val, shift_type, shift_amount, GetFlag(kCpsrC),
                             carry_out_unused, /*is_immediate_shift=*/true);
  }

  uint32_t base = ReadOperandRegister(rn);
  uint32_t transfer_address =
      pre_indexed ? (add_offset ? base + offset : base - offset) : base;

  if (load) {
    uint32_t value = byte_transfer ? memory_.Read8(transfer_address)
                                    : memory_.Read32(transfer_address);
    if (rd == kPC) {
      regs_[kPC] = value;
      pc_updated_by_instruction_ = true;
    } else {
      regs_[rd] = value;
    }
  } else {
    uint32_t value = ReadOperandRegister(rd);
    if (byte_transfer) {
      memory_.Write8(transfer_address, static_cast<uint8_t>(value));
    } else {
      memory_.Write32(transfer_address, value);
    }
  }

  if (!pre_indexed) {
    uint32_t new_base = add_offset ? base + offset : base - offset;
    regs_[rn] = new_base;
  } else if (write_back) {
    regs_[rn] = transfer_address;
  }
}

void ArmInterpreter::ExecuteBlockDataTransfer(uint32_t instr) {
  bool pre_indexed = (instr >> 24) & 1;
  bool up = (instr >> 23) & 1;
  bool write_back = (instr >> 21) & 1;
  bool load = (instr >> 20) & 1;
  uint32_t rn = (instr >> 16) & 0xF;
  uint32_t register_list = instr & 0xFFFF;

  int count = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    if ((register_list >> i) & 1) ++count;
  }

  uint32_t base = regs_[rn];
  // Standard ARM block-transfer address computation: registers always
  // land in increasing register-number -> increasing memory-address
  // order, regardless of direction; only the starting address differs
  // per addressing mode (IA/IB/DA/DB).
  uint32_t address = up ? (base + (pre_indexed ? 4 : 0))
                         : (base - count * 4 + (pre_indexed ? 0 : 4));

  for (uint32_t i = 0; i < 16; ++i) {
    if (!((register_list >> i) & 1)) continue;
    if (load) {
      uint32_t value = memory_.Read32(address);
      if (i == kPC) {
        regs_[kPC] = value;
        pc_updated_by_instruction_ = true;
      } else {
        regs_[i] = value;
      }
    } else {
      memory_.Write32(address, ReadOperandRegister(i));
    }
    address += 4;
  }

  if (write_back) {
    regs_[rn] = up ? base + count * 4 : base - count * 4;
  }
}

void ArmInterpreter::ExecuteBranchExchange(uint32_t instr) {
  // BX: cond 0001 0010 1111 1111 1111 0001 Rm
  // BLX (register): identical except bit 5 is set (link).
  bool link = (instr >> 5) & 1;
  uint32_t rm = instr & 0xF;
  uint32_t target = ReadOperandRegister(rm);
  if (target & 1) {
    throw UnimplementedInstruction(
        "BX/BLX target requests Thumb state, which isn't implemented");
  }
  if (link) {
    regs_[kLR] = regs_[kPC] + 4;
  }
  regs_[kPC] = target;
  pc_updated_by_instruction_ = true;
}

void ArmInterpreter::ExecuteHalfwordTransfer(uint32_t instr) {
  bool pre_indexed = (instr >> 24) & 1;
  bool add_offset = (instr >> 23) & 1;
  bool immediate_offset = (instr >> 22) & 1;
  bool write_back = (instr >> 21) & 1;
  bool load = (instr >> 20) & 1;
  uint32_t rn = (instr >> 16) & 0xF;
  uint32_t rd = (instr >> 12) & 0xF;
  uint32_t sh = (instr >> 5) & 0x3;

  uint32_t offset;
  if (immediate_offset) {
    offset = (((instr >> 8) & 0xF) << 4) | (instr & 0xF);
  } else {
    uint32_t rm = instr & 0xF;
    offset = ReadOperandRegister(rm);
  }

  uint32_t base = ReadOperandRegister(rn);
  uint32_t transfer_address =
      pre_indexed ? (add_offset ? base + offset : base - offset) : base;

  if (load) {
    uint32_t value;
    switch (sh) {
      case 1:  // LDRH: zero-extended halfword
        value = memory_.Read16(transfer_address);
        break;
      case 2: {  // LDRSB: sign-extended byte
        auto b = static_cast<int8_t>(memory_.Read8(transfer_address));
        value = static_cast<uint32_t>(static_cast<int32_t>(b));
        break;
      }
      case 3: {  // LDRSH: sign-extended halfword
        auto h = static_cast<int16_t>(memory_.Read16(transfer_address));
        value = static_cast<uint32_t>(static_cast<int32_t>(h));
        break;
      }
      default:
        throw UnimplementedInstruction(
            "Halfword transfer with SH=00 (multiply/swap space)");
    }
    if (rd == kPC) {
      regs_[kPC] = value;
      pc_updated_by_instruction_ = true;
    } else {
      regs_[rd] = value;
    }
  } else {
    if (sh != 1) {
      throw UnimplementedInstruction(
          "Halfword transfer store with SH != 01 (not a valid encoding)");
    }
    memory_.Write16(transfer_address,
                     static_cast<uint16_t>(ReadOperandRegister(rd)));
  }

  if (!pre_indexed) {
    uint32_t new_base = add_offset ? base + offset : base - offset;
    regs_[rn] = new_base;
  } else if (write_back) {
    regs_[rn] = transfer_address;
  }
}

void ArmInterpreter::Step() {
  uint32_t fetch_addr = regs_[kPC];
  if (call_out_size_ != 0 && fetch_addr >= call_out_base_ &&
      fetch_addr < call_out_base_ + call_out_size_) {
    if (call_out_handler_) {
      call_out_handler_(*this, fetch_addr);
    }
    return;
  }

  uint32_t instr = memory_.Read32(fetch_addr);
  pc_updated_by_instruction_ = false;

  uint32_t cond = (instr >> 28) & 0xF;
  if (ConditionPassed(cond)) {
    uint32_t bits27_25 = (instr >> 25) & 0x7;

    if (bits27_25 == 0b101) {
      ExecuteBranch(instr);
    } else if (bits27_25 == 0b011) {
      if ((instr & 0x10) != 0) {
        throw UnimplementedInstruction("Undefined instruction space");
      }
      ExecuteSingleDataTransfer(instr);
    } else if (bits27_25 == 0b010) {
      ExecuteSingleDataTransfer(instr);
    } else if (bits27_25 == 0b000 || bits27_25 == 0b001) {
      uint32_t opcode = (instr >> 21) & 0xF;
      uint32_t s_bit = (instr >> 20) & 1;
      uint32_t bit25 = (instr >> 25) & 1;
      uint32_t bit4 = (instr >> 4) & 1;
      uint32_t bit7 = (instr >> 7) & 1;

      // Must be checked BEFORE the opcode/S-bit "miscellaneous
      // instructions" reassignment below: bits[24:21] only mean "opcode"
      // for true data-processing-shaped instructions. Multiply/halfword
      // encodings reuse those same bit positions for P/U/I/W-type fields
      // that can numerically collide with the 0x8-0xB opcode range
      // (e.g. STRH's immediate/down-indexed form does exactly this) --
      // checking bit4/bit7 first routes those correctly regardless.
      bool is_test_opcode = opcode >= 0x8 && opcode <= 0xB;
      if (bit25 == 0 && bit4 == 1 && bit7 == 1) {
        // Shared bit pattern for multiply/swap (SH=00) and halfword/
        // signed transfer (SH=01/10/11) -- only the former stays
        // unimplemented.
        uint32_t sh = (instr >> 5) & 0x3;
        if (sh == 0) {
          throw UnimplementedInstruction("Multiply/swap instruction space");
        }
        ExecuteHalfwordTransfer(instr);
      } else if (is_test_opcode && s_bit == 0) {
        // This opcode range with S=0 is reassigned to the "miscellaneous
        // instructions" space (MRS/MSR/BX/BLX/CLZ/BXJ/...), not a real
        // TST/TEQ/CMP/CMN. BX/BLX Rm's bit pattern is:
        // cond 0001 0010 1111 1111 1111 001L Rm (L = link bit, bit 5) --
        // everything else in this space stays unimplemented.
        if ((instr & 0x0FFFFFD0) == 0x012FFF10) {
          ExecuteBranchExchange(instr);
        } else {
          throw UnimplementedInstruction(
              "Miscellaneous instruction space (MRS/MSR/etc.)");
        }
      } else {
        ExecuteDataProcessing(instr);
      }
    } else if (bits27_25 == 0b100) {
      bool s_bit = (instr >> 22) & 1;
      if (s_bit) {
        throw UnimplementedInstruction(
            "Block data transfer with S=1 (user-bank registers / "
            "exception return) not supported");
      }
      ExecuteBlockDataTransfer(instr);
    } else {
      throw UnimplementedInstruction("Coprocessor instruction / SWI");
    }
  }

  if (!pc_updated_by_instruction_) {
    regs_[kPC] = fetch_addr + 4;
  }
}

uint64_t ArmInterpreter::Run(uint64_t max_instructions) {
  uint64_t executed = 0;
  while (executed < max_instructions) {
    uint32_t fetch_addr = regs_[kPC];
    bool will_trap = call_out_size_ != 0 && fetch_addr >= call_out_base_ &&
                      fetch_addr < call_out_base_ + call_out_size_;
    Step();
    ++executed;
    if (will_trap) break;
  }
  return executed;
}

}  // namespace zeebulator
