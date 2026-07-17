#pragma once

#include <cstddef>
#include <cstdint>

#include "core/brew/hle_runtime.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Builds a generic BREW interface object with `slot_count` vtable slots,
// every one of which just sets r0=0 and returns. Real disassembly of
// Double Dragon (TASKS.md Phase 8) shows it calling
// ISHELL_CreateInstance for real BREW classes we haven't identified
// (e.g. ClsId 0x01002001) and then unconditionally invoking specific
// vtable slots on the result (e.g. slot 33) with no way to know the
// real interface's shape without guessing. Rather than guess a real
// interface (risking silently-wrong behavior) or leave CreateInstance
// failing (the already-diagnosed "memory insufficient" dead end), this
// gives the app a harmless object that satisfies "CreateInstance
// succeeded, calling any of its methods is safe" -- used to empirically
// observe what the game does next, then replaced with real behavior for
// whichever slots turn out to matter.
uint32_t BuildGenericStubObject(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                                 uint32_t object_address, size_t slot_count);

}  // namespace zeebulator
