#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/loader/bar.h"
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
// LoadResDataEx (slot 41) is real too, backed by RegisterResourceFile:
// confirmed end to end against real Peggle (`peggle.mod`, TASKS.md
// Phase 8/PHASE8_LOG.md) -- a full instruction trace of a real per-tick
// call found the exact real `(pIShell, pszResFile, wResID, resType,
// pBuffer, pnLen)` calling convention, the real `-1` buffer sentinel
// for a size-only query (matching the real documented
// `ISHELL_GetResSize` macro), and, via `core/loader/bar.h`'s own
// resource-ID directory, exactly which real `.bar` entry a given
// `(resType, wResID)` pair resolves to.
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
  // `screen_width`/`screen_height` default to Zeebo's one real native
  // resolution (640x480, the same constant every frontend already
  // hardcodes) -- real GetDeviceInfo (slot 4) needs them to answer real
  // `AEEDeviceInfo::cxScreen`/`cyScreen` queries; real code that never
  // calls GetDeviceInfo doesn't need this, hence the default rather than
  // a required constructor argument.
  IShellHle(Memory& memory, HleRuntime& hle, int screen_width = 640, int screen_height = 480);

  // Registers the object pointer ISHELL_CreateInstance should hand back
  // for `cls_id`. Must be called before Build(), for any class the app
  // is expected to successfully create.
  void RegisterInstance(uint32_t cls_id, uint32_t object_ptr);

  // Registers a real `.bar` resource file's raw bytes under the real
  // filename `ISHELL_LoadResDataEx` requests it by (e.g.
  // `"resources.bar"`), so real slot 41 calls can serve real resource
  // data instead of a blind stub. Parses `data` immediately (throws
  // std::runtime_error on a malformed archive, same contract as
  // `BarArchive::Parse` -- see core/loader/bar.h). Optional: games that
  // never call `LoadResDataEx` don't need this.
  void RegisterResourceFile(const std::string& name, std::vector<uint8_t> data);

  uint32_t Build(uint32_t vtable_address, uint32_t object_address);

  // Schedules `callback`/`user_data` exactly the way a real
  // ISHELL_SetTimer(ms, callback, user_data) call would (same re-arm-
  // by-re-registering-the-same-identity semantics as SetTimerImpl),
  // without needing to fake a full ARM call through the real trap.
  // EXPERIMENTAL, added for one specific real, evidenced case (TASKS.md
  // Phase 8, Super BurgerTime): a real per-frame update function
  // reached via a still-unidentified class's method, not through
  // ISHELL_SetTimer directly -- see tools/game_probe.cpp for the real
  // disassembly evidence this is grounded in.
  void ScheduleTimer(uint32_t ms, uint32_t callback, uint32_t user_data);

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
  void GetDeviceInfoImpl(IArmCore& core);
  void SetTimerImpl(IArmCore& core);
  void CancelTimerImpl(IArmCore& core);
  void LoadResDataExImpl(IArmCore& core);

  Memory& memory_;
  HleRuntime& hle_;
  int screen_width_;
  int screen_height_;
  std::unordered_map<uint32_t, uint32_t> instances_;
  std::vector<PendingTimer> timers_;
  std::unordered_map<std::string, BarArchive> resource_files_;
};

}  // namespace zeebulator
