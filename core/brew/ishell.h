#pragma once

#include <cstdint>

#include "core/brew/hle_runtime.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Builds an IShell HLE object. Vtable slot order verified directly
// against Qualcomm's own AEEIShell.h (see TASKS.md Phase 3) -- AddRef
// and Release (from IBase) followed by CreateInstance through
// AppIsInGroup, append-only across BREW SDK versions.
//
// Every slot is currently a safe stub (sets a zero/failure-ish return
// value, no other side effects) -- nothing in scope calls any IShell
// method yet (the M0 test app gets its IDisplay pointer directly via the
// EVT_APP_START event, not through IShell), but the vtable still needs a
// valid entry in every slot in case real game code calls one
// unexpectedly. Extend individual slots with real behavior as games
// need them.
uint32_t BuildIShell(Memory& memory, HleRuntime& hle, uint32_t vtable_address,
                      uint32_t object_address);

}  // namespace zeebulator
