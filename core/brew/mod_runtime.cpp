#include "core/brew/mod_runtime.h"

namespace zeebulator {

namespace {
// Offsets within the static-base table where real disassembly (see
// mod_runtime.h) shows the MALLOC/FREE/GetAppContext function pointers
// living.
constexpr uint32_t kMemsetSlotOffset = 0x4;
constexpr uint32_t kStrlenSlotOffset = 0x14;
constexpr uint32_t kMallocSlotOffset = 0x68;
constexpr uint32_t kFreeSlotOffset = 0x6c;
constexpr uint32_t kGetUpTimeMsSlotOffset = 0xb0;
constexpr uint32_t kGetAppContextSlotOffset = 0xc0;
// Offsets within the "app context" struct GetAppContext returns where
// real call sites read the current app's IShell/IDisplay pointers.
constexpr uint32_t kAppContextShellOffset = 12;
constexpr uint32_t kAppContextDisplayOffset = 20;
}  // namespace

ModRuntime::ModRuntime(Memory& memory, HleRuntime& hle, uint32_t heap_region, uint32_t heap_size,
                        uint32_t context_address)
    : memory_(memory),
      hle_(hle),
      heap_cursor_(heap_region),
      heap_end_(heap_region + heap_size),
      context_address_(context_address) {}

void ModRuntime::SetShellInstance(uint32_t shell_ptr) { shell_ptr_ = shell_ptr; }

void ModRuntime::SetDisplayInstance(uint32_t display_ptr) { display_ptr_ = display_ptr; }

void ModRuntime::MallocImpl(IArmCore& core) {
  uint32_t size = core.GetRegister(kR0);
  uint32_t aligned = (size + 3) & ~3u;  // word-align every allocation
  if (aligned > heap_end_ - heap_cursor_) {
    core.SetRegister(kR0, 0);  // NULL: out of (emulated) heap space
    return;
  }
  uint32_t result = heap_cursor_;
  heap_cursor_ += aligned;
  core.SetRegister(kR0, result);
}

void ModRuntime::MemsetImpl(IArmCore& core) {
  // void *memset(void *s, int c, size_t n)
  uint32_t dest = core.GetRegister(kR0);
  uint8_t value = static_cast<uint8_t>(core.GetRegister(kR1));
  uint32_t count = core.GetRegister(kR2);
  for (uint32_t i = 0; i < count; ++i) {
    memory_.Write8(dest + i, value);
  }
  core.SetRegister(kR0, dest);  // memset returns its first argument
}

void ModRuntime::StrlenImpl(IArmCore& core) {
  // size_t strlen(const char *s)
  uint32_t s = core.GetRegister(kR0);
  uint32_t len = 0;
  while (memory_.Read8(s + len) != 0) {
    ++len;
  }
  core.SetRegister(kR0, len);
}

void ModRuntime::GetAppContextImpl(IArmCore& core) {
  // Written fresh on every call rather than once in Install() so this
  // works regardless of whether SetShellInstance() is called before or
  // after Install().
  memory_.Write32(context_address_ + kAppContextShellOffset, shell_ptr_);
  memory_.Write32(context_address_ + kAppContextDisplayOffset, display_ptr_);
  core.SetRegister(kR0, context_address_);
}

void ModRuntime::GetUpTimeMsImpl(IArmCore& core) { core.SetRegister(kR0, uptime_ms_); }

void ModRuntime::Tick(uint32_t elapsed_ms) { uptime_ms_ += elapsed_ms; }

void ModRuntime::Install(uint32_t module_base, uint32_t table_address) {
  uint32_t memset_fn = hle_.Register([this](IArmCore& core) { MemsetImpl(core); });
  uint32_t strlen_fn = hle_.Register([this](IArmCore& core) { StrlenImpl(core); });
  uint32_t malloc_fn = hle_.Register([this](IArmCore& core) { MallocImpl(core); });
  uint32_t free_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t get_uptime_ms_fn = hle_.Register([this](IArmCore& core) { GetUpTimeMsImpl(core); });
  uint32_t get_app_context_fn = hle_.Register([this](IArmCore& core) { GetAppContextImpl(core); });
  memory_.Write32(table_address + kMemsetSlotOffset, memset_fn);
  memory_.Write32(table_address + kStrlenSlotOffset, strlen_fn);
  memory_.Write32(table_address + kMallocSlotOffset, malloc_fn);
  memory_.Write32(table_address + kFreeSlotOffset, free_fn);
  memory_.Write32(table_address + kGetUpTimeMsSlotOffset, get_uptime_ms_fn);
  memory_.Write32(table_address + kGetAppContextSlotOffset, get_app_context_fn);
  memory_.Write32(module_base - 4, table_address);
}

}  // namespace zeebulator
