#pragma once

#include <cstdint>

#include "core/brew/hle_runtime.h"
#include "core/memory/memory.h"

namespace zeebulator {

// Wires up the ARM RVCT "ROPI" (Read-Only Position Independent) static-
// base convention real compiled .mod code expects. See TASKS.md Phase 8
// for how this was found: real disassembly of Double Dragon's real
// ddragonz.mod (arm-none-eabi-objdump, not manual hex decoding) shows
// AEEStaticMod_New -- the standard AEEModGen.c helper every module's
// AEEMod_Load calls -- computing its own load address via PC-relative
// addressing, then reading a pointer from 4 bytes *before* that address,
// then calling a function pointer read from offset 0x68 within whatever
// that points to, passing one integer argument (a byte count) and
// expecting a pointer back. That matches AEEModGen.c's own
// `MALLOC(nSize + sizeof(IModuleVtbl))` call exactly -- IModuleVtbl is 4
// function pointers (16 bytes on this ABI), and the disassembly computes
// r0 = nSize + 16 immediately before that indirect call.
//
// A second slot, offset 0x6c (immediately after MALLOC), is FREE: real
// disassembly of AEEApplet_New's cleanup path (called when
// ISHELL_CreateInstance(AEECLSID_DISPLAY) fails, TASKS.md Phase 8) reads
// a function pointer from there and calls it with one pointer argument,
// matching the reference source's `FREE(pme)` exactly.
//
// Only these two table slots are confirmed by real disassembly so far.
// Every other offset is left unmapped -- a real .mod hitting one would
// fetch from unwritten memory, which tools/game_probe.cpp's
// wandered-outside-module check exists specifically to catch and report
// loudly rather than silently misbehave.
class ModRuntime {
 public:
  // `heap_region`/`heap_size` bound a simple bump allocator the MALLOC
  // slot hands memory out from. FREE is a no-op (leaks) -- consistent
  // with having no free-list, which is fine for a single game-session
  // emulator run.
  ModRuntime(Memory& memory, HleRuntime& hle, uint32_t heap_region, uint32_t heap_size);

  // Writes `table_address` at `module_base - 4` and populates the
  // table's one known slot. Must be called after the module itself has
  // been loaded at `module_base`; `table_address` must not overlap the
  // module or any other memory region in use.
  void Install(uint32_t module_base, uint32_t table_address);

 private:
  void MallocImpl(IArmCore& core);

  Memory& memory_;
  HleRuntime& hle_;
  uint32_t heap_cursor_;
  uint32_t heap_end_;
};

}  // namespace zeebulator
