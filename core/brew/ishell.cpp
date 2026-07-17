#include "core/brew/ishell.h"

#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {

// Generic stub: sets a zero/failure-ish return value and does nothing
// else. Safe default for any BREW method call whose behavior isn't
// implemented yet -- real games that hit one of these will need it
// filled in with real behavior at that point.
void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }

}  // namespace

IShellHle::IShellHle(Memory& memory, HleRuntime& hle) : memory_(memory), hle_(hle) {}

void IShellHle::RegisterInstance(uint32_t cls_id, uint32_t object_ptr) {
  instances_[cls_id] = object_ptr;
}

void IShellHle::CreateInstanceImpl(IArmCore& core) {
  // int CreateInstance(IShell *po, AEECLSID cls, void **ppo)
  uint32_t cls_id = core.GetRegister(kR1);
  uint32_t ppobj = core.GetRegister(kR2);
  auto it = instances_.find(cls_id);
  if (it == instances_.end()) {
    core.SetRegister(kR0, 1);  // EFAILED-ish: unknown/unimplemented class
    return;
  }
  memory_.Write32(ppobj, it->second);
  core.SetRegister(kR0, 0);  // SUCCESS
}

uint32_t IShellHle::Build(uint32_t vtable_address, uint32_t object_address) {
  // Order matches AEEIShell.h's INHERIT_IShell macro exactly (verified
  // directly against real Qualcomm source -- see TASKS.md Phase 3), up
  // through the pre-BREW-MP slot count (40 IShell-specific methods,
  // which is what a 2009-era Zeebo/BREW 4.x IShell should have --
  // BREW MP's later-appended slots, e.g. RegisterSystemCallback onward,
  // are deliberately not included since Zeebo predates that rebrand).
  std::vector<HleRuntime::HleFunction> methods = {
      Stub,                                            // 0  AddRef
      Stub,                                            // 1  Release
      [this](IArmCore& c) { CreateInstanceImpl(c); },   // 2  CreateInstance
      Stub,  // 3  QueryClass
      Stub,  // 4  GetDeviceInfo
      Stub,  // 5  StartApplet
      Stub,  // 6  CloseApplet
      Stub,  // 7  CanStartApplet
      Stub,  // 8  ActiveApplet
      Stub,  // 9  EnumAppletInit
      Stub,  // 10 EnumNextApplet
      Stub,  // 11 SetTimer
      Stub,  // 12 CancelTimer
      Stub,  // 13 GetTimerExpiration
      Stub,  // 14 CreateDialog
      Stub,  // 15 GetActiveDialog
      Stub,  // 16 EndDialog
      Stub,  // 17 LoadResString
      Stub,  // 18 LoadResData
      Stub,  // 19 LoadResObject
      Stub,  // 20 FreeResData
      Stub,  // 21 SendEvent
      Stub,  // 22 Beep
      Stub,  // 23 GetPrefs
      Stub,  // 24 SetPrefs
      Stub,  // 25 GetItemStyle
      Stub,  // 26 Prompt
      Stub,  // 27 MessageBox
      Stub,  // 28 MessageBoxText
      Stub,  // 29 SetAlarm
      Stub,  // 30 CancelAlarm
      Stub,  // 31 AlarmsActive
      Stub,  // 32 GetHandler
      Stub,  // 33 RegisterHandler
      Stub,  // 34 RegisterNotify
      Stub,  // 35 Notify
      Stub,  // 36 Resume
      Stub,  // 37 ForceExit
      Stub,  // 38 GetPosition
      Stub,  // 39 CheckPrivLevel
      Stub,  // 40 IsValidResource
      Stub,  // 41 LoadResDataEx
  };
  return BuildInterfaceObject(memory_, hle_, vtable_address, object_address, methods);
}

}  // namespace zeebulator
