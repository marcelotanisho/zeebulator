#include "core/brew/ishell.h"

#include <utility>

#include "core/brew/interface_object.h"

namespace zeebulator {

namespace {

// Generic stub: sets a zero/failure-ish return value and does nothing
// else. Safe default for any BREW method call whose behavior isn't
// implemented yet -- real games that hit one of these will need it
// filled in with real behavior at that point.
void Stub(IArmCore& core) { core.SetRegister(kR0, 0); }

std::string ReadCString(Memory& memory, uint32_t addr) {
  std::string s;
  for (uint8_t c = memory.Read8(addr); c != 0; c = memory.Read8(++addr)) {
    s.push_back(static_cast<char>(c));
  }
  return s;
}

}  // namespace

IShellHle::IShellHle(Memory& memory, HleRuntime& hle, int screen_width, int screen_height)
    : memory_(memory), hle_(hle), screen_width_(screen_width), screen_height_(screen_height) {}

void IShellHle::RegisterInstance(uint32_t cls_id, uint32_t object_ptr) {
  instances_[cls_id] = object_ptr;
}

void IShellHle::RegisterResourceFile(const std::string& name, std::vector<uint8_t> data) {
  resource_files_.emplace(name, BarArchive::Parse(std::move(data)));
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

void IShellHle::GetDeviceInfoImpl(IArmCore& core) {
  // void GetDeviceInfo(IShell *po, AEEDeviceInfo *pdi)
  // po is R0 (unused, matches "this"), pdi R1. Real AEEDeviceInfo (see
  // the real bundled AEEShell.h reference, research/docs/
  // sdk_installer_extract/brew_sdk_headers_reference/) starts with
  // `uint16 cxScreen; uint16 cyScreen;` at offsets 0/2 -- confirmed
  // against real Double Dragon disassembly (TASKS.md/PHASE8_LOG.md
  // Phase 8): its own real screen-size-query call site reads exactly
  // those two offsets out of this call's own output buffer, and (before
  // this fix) got 0/0 back from this slot's old blind-stub behavior,
  // producing a real, confirmed-via-live-GL-trace degenerate
  // `glViewport(0,0,1,0)` call downstream. Every other real
  // AEEDeviceInfo field is left zeroed (this project doesn't have
  // evidence any real game reads them yet) -- not a claim the rest of
  // the struct is correctly populated, just that these two confirmed-
  // read fields are.
  uint32_t pdi = core.GetRegister(kR1);
  if (pdi != 0) {
    memory_.Write16(pdi + 0, static_cast<uint16_t>(screen_width_));   // cxScreen
    memory_.Write16(pdi + 2, static_cast<uint16_t>(screen_height_));  // cyScreen
  }
  core.SetRegister(kR0, 0);
}

void IShellHle::ScheduleTimer(uint32_t ms, uint32_t callback, uint32_t user_data) {
  // Re-registering the same (callback, user_data) pair reschedules it
  // rather than creating a duplicate -- matches the real self-rearming
  // timer pattern (see class doc comment) where the callback calls
  // SetTimer again with the same identity every time it fires.
  for (auto& timer : timers_) {
    if (timer.callback == callback && timer.user_data == user_data) {
      timer.remaining_ms = ms;
      return;
    }
  }
  timers_.push_back(PendingTimer{ms, callback, user_data});
}

void IShellHle::SetTimerImpl(IArmCore& core) {
  // int SetTimer(IShell *ps, uint32 dwCount, PFNNOTIFY pfnNotify, void *pUser)
  ScheduleTimer(core.GetRegister(kR1), core.GetRegister(kR2), core.GetRegister(kR3));
  core.SetRegister(kR0, 0);  // SUCCESS
}

void IShellHle::CancelTimerImpl(IArmCore& core) {
  // int CancelTimer(IShell *ps, PFNNOTIFY pfnNotify, void *pUser)
  uint32_t callback = core.GetRegister(kR1);
  uint32_t user_data = core.GetRegister(kR2);
  for (auto it = timers_.begin(); it != timers_.end(); ++it) {
    if (it->callback == callback && it->user_data == user_data) {
      timers_.erase(it);
      core.SetRegister(kR0, 0);  // SUCCESS
      return;
    }
  }
  core.SetRegister(kR0, 1);  // EFAILED-ish: no matching timer
}

void IShellHle::LoadResDataExImpl(IArmCore& core) {
  // AEEResult LoadResDataEx(IShell *pIShell, const char *pszResFile,
  //   uint16 wResID, AEERESTYPE resType, void *pBuffer, uint32 *pnLen)
  // Real calling convention and the real `(void*)-1` "size only" buffer
  // sentinel confirmed against a real Peggle call site -- see this
  // class's own doc comment.
  std::string filename = ReadCString(memory_, core.GetRegister(kR1));
  uint32_t id = core.GetRegister(kR2);
  uint32_t type = core.GetRegister(kR3);
  uint32_t buffer = HleRuntime::ReadStackArg(core, 0);
  uint32_t len_addr = HleRuntime::ReadStackArg(core, 1);

  auto file_it = resource_files_.find(filename);
  if (file_it == resource_files_.end()) {
    core.SetRegister(kR0, 1);  // EFAILED-ish: unregistered resource file
    return;
  }
  const BarEntry* entry =
      file_it->second.Find(static_cast<uint16_t>(type), static_cast<uint16_t>(id));
  if (entry == nullptr) {
    core.SetRegister(kR0, 1);  // EFAILED-ish: no directory entry for this (type, id)
    return;
  }

  constexpr uint32_t kSizeOnlySentinel = 0xFFFFFFFF;
  if (buffer == kSizeOnlySentinel) {
    if (len_addr != 0) memory_.Write32(len_addr, entry->size);
    core.SetRegister(kR0, 0);  // SUCCESS
    return;
  }

  std::vector<uint8_t> data = file_it->second.Extract(*entry);
  for (uint32_t i = 0; i < data.size(); ++i) {
    memory_.Write8(buffer + i, data[i]);
  }
  if (len_addr != 0) memory_.Write32(len_addr, entry->size);
  core.SetRegister(kR0, 0);  // SUCCESS
}

std::vector<IShellHle::ExpiredTimer> IShellHle::Tick(uint32_t elapsed_ms) {
  std::vector<ExpiredTimer> expired;
  for (auto it = timers_.begin(); it != timers_.end();) {
    if (elapsed_ms >= it->remaining_ms) {
      expired.push_back(ExpiredTimer{it->callback, it->user_data});
      it = timers_.erase(it);
    } else {
      it->remaining_ms -= elapsed_ms;
      ++it;
    }
  }
  return expired;
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
      [this](IArmCore& c) { GetDeviceInfoImpl(c); },  // 4  GetDeviceInfo
      Stub,  // 5  StartApplet
      Stub,  // 6  CloseApplet
      Stub,  // 7  CanStartApplet
      Stub,  // 8  ActiveApplet
      Stub,  // 9  EnumAppletInit
      Stub,  // 10 EnumNextApplet
      [this](IArmCore& c) { SetTimerImpl(c); },     // 11 SetTimer
      [this](IArmCore& c) { CancelTimerImpl(c); },  // 12 CancelTimer
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
      [this](IArmCore& c) { LoadResDataExImpl(c); },  // 41 LoadResDataEx
      // Slots below this point are NOT verified against any real header --
      // unlike 0-41 above, they're only known to exist at all because real
      // Double Dragon disassembly (`ddragonz.mod` offset 0x10a234) showed a
      // genuine call through vtable offset 0xac (slot 43), one word past
      // what the "40 IShell-specific methods" pre-BREW-MP count above
      // accounted for -- either that count was of an incomplete real
      // header, or Zeebo's own BREW variant extends classic IShell by a
      // couple of slots the same way this project has already found it
      // doing for other interfaces. Extended with safe, generously-sized
      // headroom (matching this project's established precedent, e.g. the
      // HID device scaffold) rather than pinned exactly to slot 43, so the
      // next real call into this range doesn't reproduce the same
      // undersized-vtable crash.
      Stub,  // 42 unconfirmed
      Stub,  // 43 unconfirmed -- the one real call site found so far
      Stub,  // 44 unconfirmed
      Stub,  // 45 unconfirmed
      Stub,  // 46 unconfirmed
      Stub,  // 47 unconfirmed
      Stub,  // 48 unconfirmed
      Stub,  // 49 unconfirmed
  };
  return BuildInterfaceObject(memory_, hle_, vtable_address, object_address, methods);
}

}  // namespace zeebulator
