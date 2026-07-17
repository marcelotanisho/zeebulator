#include "core/brew/mod_runtime.h"

namespace zeebulator {

namespace {
// Offset within the static-base table where real disassembly (see
// mod_runtime.h) shows the MALLOC-equivalent function pointer living.
constexpr uint32_t kMallocSlotOffset = 0x68;
}  // namespace

ModRuntime::ModRuntime(Memory& memory, HleRuntime& hle, uint32_t heap_region, uint32_t heap_size)
    : memory_(memory), hle_(hle), heap_cursor_(heap_region), heap_end_(heap_region + heap_size) {}

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

void ModRuntime::Install(uint32_t module_base, uint32_t table_address) {
  uint32_t malloc_fn = hle_.Register([this](IArmCore& core) { MallocImpl(core); });
  memory_.Write32(table_address + kMallocSlotOffset, malloc_fn);
  memory_.Write32(module_base - 4, table_address);
}

}  // namespace zeebulator
