#pragma once

#include <cstdint>
#include <unordered_map>

#include "core/brew/hle_runtime.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Builds an IShell HLE object. Vtable slot order verified directly
// against Qualcomm's own AEEIShell.h (see TASKS.md Phase 3) -- AddRef
// and Release (from IBase) followed by CreateInstance through
// AppIsInGroup, append-only across BREW SDK versions.
//
// Every slot except CreateInstance is currently a safe stub (sets a
// zero/failure-ish return value, no other side effects) -- extend
// individual slots with real behavior as games need them.
//
// CreateInstance is real, backed by a small registry of known singleton
// instances (see RegisterInstance): real compiled BREW app code goes
// through ISHELL_CreateInstance to obtain interfaces like IDisplay
// rather than receiving them directly in all cases -- confirmed via real
// disassembly of Double Dragon's AEEApplet_New call chain (TASKS.md
// Phase 8), which calls `ISHELL_CreateInstance(pIShell,
// AEECLSID_DISPLAY, &m_pIDisplay)` and treats a null result as fatal. An
// unregistered ClsId returns EFAILED (1), matching real BREW behavior
// for an unrecognized/unimplemented class rather than a lying "success"
// with an unwritten output pointer.
class IShellHle {
 public:
  IShellHle(Memory& memory, HleRuntime& hle);

  // Registers the object pointer ISHELL_CreateInstance should hand back
  // for `cls_id`. Must be called before Build(), for any class the app
  // is expected to successfully create.
  void RegisterInstance(uint32_t cls_id, uint32_t object_ptr);

  uint32_t Build(uint32_t vtable_address, uint32_t object_address);

 private:
  void CreateInstanceImpl(IArmCore& core);

  Memory& memory_;
  HleRuntime& hle_;
  std::unordered_map<uint32_t, uint32_t> instances_;
};

}  // namespace zeebulator
