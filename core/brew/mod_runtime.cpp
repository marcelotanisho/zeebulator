#include "core/brew/mod_runtime.h"

namespace zeebulator {

namespace {
// Offsets within the static-base table where real disassembly (see
// mod_runtime.h) shows the MALLOC/FREE/GetAppContext function pointers
// living.
constexpr uint32_t kMallocSlotOffset = 0x68;
constexpr uint32_t kFreeSlotOffset = 0x6c;
constexpr uint32_t kGetAppContextSlotOffset = 0xc0;
// Offset within the "app context" struct GetAppContext returns where
// real call sites read the current app's IShell pointer.
constexpr uint32_t kAppContextShellOffset = 12;
}  // namespace

ModRuntime::ModRuntime(Memory& memory, HleRuntime& hle, uint32_t heap_region, uint32_t heap_size,
                        uint32_t context_address)
    : memory_(memory),
      hle_(hle),
      heap_cursor_(heap_region),
      heap_end_(heap_region + heap_size),
      context_address_(context_address) {}

void ModRuntime::SetShellInstance(uint32_t shell_ptr) { shell_ptr_ = shell_ptr; }

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

void ModRuntime::GetAppContextImpl(IArmCore& core) {
  // Written fresh on every call rather than once in Install() so this
  // works regardless of whether SetShellInstance() is called before or
  // after Install().
  memory_.Write32(context_address_ + kAppContextShellOffset, shell_ptr_);
  core.SetRegister(kR0, context_address_);
}

void ModRuntime::Install(uint32_t module_base, uint32_t table_address) {
  uint32_t malloc_fn = hle_.Register([this](IArmCore& core) { MallocImpl(core); });
  uint32_t free_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t get_app_context_fn = hle_.Register([this](IArmCore& core) { GetAppContextImpl(core); });
  memory_.Write32(table_address + kMallocSlotOffset, malloc_fn);
  memory_.Write32(table_address + kFreeSlotOffset, free_fn);
  memory_.Write32(table_address + kGetAppContextSlotOffset, get_app_context_fn);
  memory_.Write32(module_base - 4, table_address);
}

}  // namespace zeebulator
