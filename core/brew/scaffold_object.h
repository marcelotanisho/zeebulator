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

// Same as BuildGenericStubObject, but with one slot replaced by a real
// implementation. Real disassembly of Double Dragon (TASKS.md Phase 8)
// shows the object IDisplay::GetDeviceBitmap returns being used for
// exactly one meaningful thing -- a "QueryInterface"-shaped call at
// slot 2 (`obj->vtable[2](obj, clsid, &ppo)`, same calling convention
// as ISHELL_CreateInstance) -- while every other slot is unused. This
// lets that one slot get a real (if still generic) implementation
// without guessing the rest of the interface's shape.
uint32_t BuildStubObjectWithOverride(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                                      uint32_t object_address, size_t slot_count,
                                      size_t override_slot, HleRuntime::HleFunction override_fn);

}  // namespace zeebulator
