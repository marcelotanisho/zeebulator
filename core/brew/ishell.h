#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Builds an IShell HLE object. Vtable slot order verified directly
// against Qualcomm's own AEEIShell.h (see TASKS.md Phase 3) -- AddRef
// and Release (from IBase) followed by CreateInstance through
// AppIsInGroup, append-only across BREW SDK versions.
//
// Every slot except CreateInstance/SetTimer/CancelTimer is currently a
// safe stub (sets a zero/failure-ish return value, no other side
// effects) -- extend individual slots with real behavior as games need
// them.
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
//
// SetTimer/CancelTimer are real too: real BREW timers are one-shot, not
// repeating -- confirmed against a real bundled SDK sample
// (`research/samples/conftest_source/conftest/conftest.c`), whose own
// `PFNNOTIFY` timer callback re-arms itself by calling
// `ISHELL_SetTimer(pIShell, GAMELOOP_TIMER_MS, callback, pMe)` again as
// its own last action -- the standard BREW "game loop via self-
// rearming timer" pattern real Double Dragon uses too (confirmed via
// real disassembly of its HandleEvent(EVT_APP_START), see TASKS.md
// Phase 8). This class only tracks pending timers; it doesn't call ARM
// code itself -- see Tick().
class IShellHle {
 public:
  IShellHle(Memory& memory, HleRuntime& hle);

  // Registers the object pointer ISHELL_CreateInstance should hand back
  // for `cls_id`. Must be called before Build(), for any class the app
  // is expected to successfully create.
  void RegisterInstance(uint32_t cls_id, uint32_t object_ptr);

  uint32_t Build(uint32_t vtable_address, uint32_t object_address);

  // A pending SetTimer callback whose deadline Tick() determined has now
  // been reached. The caller is responsible for actually invoking it
  // (e.g. via HleRuntime::CallArmFunction(callback, user_data)) -- this
  // class has no CPU access of its own.
  struct ExpiredTimer {
    uint32_t callback;
    uint32_t user_data;
  };

  // Advances every pending timer by `elapsed_ms` and returns (removing)
  // any that have now reached their deadline, in the order they were
  // originally registered.
  std::vector<ExpiredTimer> Tick(uint32_t elapsed_ms);

 private:
  struct PendingTimer {
    uint32_t remaining_ms;
    uint32_t callback;
    uint32_t user_data;
  };

  void CreateInstanceImpl(IArmCore& core);
  void SetTimerImpl(IArmCore& core);
  void CancelTimerImpl(IArmCore& core);

  Memory& memory_;
  HleRuntime& hle_;
  std::unordered_map<uint32_t, uint32_t> instances_;
  std::vector<PendingTimer> timers_;
};

}  // namespace zeebulator
