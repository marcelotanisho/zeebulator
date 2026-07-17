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
// A third slot, offset 0xc0, is by far the most heavily used (138
// distinct real call sites in ddragonz.mod alone, found by scanning the
// whole binary for the "read static base, index table" instruction
// pattern -- see TASKS.md Phase 8). Every site follows an identical
// shape: call this slot with no meaningful argument, twice in a row
// (compiler doesn't common-subexpression-eliminate it, so it's genuinely
// called twice), read offset+12 from the returned pointer both times,
// dereference that once more to reach a vtable, and call vtable slot 2
// (CreateInstance) on it. That's exactly the shape of
// `ISHELL_CreateInstance(pIShell, ClsId, ppObj)` where `pIShell` is
// fetched from an ambient "current app context" rather than passed in
// explicitly -- the mechanism real AEEStdLib helper macros that don't
// take an IShell parameter rely on internally. `SetShellInstance()`
// supplies the real pointer this slot should expose.
//
// The same context struct has a second confirmed field, offset +20
// (sibling to the IShell pointer at +12): a real call site
// (`ddragonz.mod` offset 0x1b5e0) reads it and stores it directly into
// a field inside the applet's own struct, which real code elsewhere
// (offset 0x23a18) later dereferences and calls vtable slot 18 on with
// `pRect=NULL` -- matching real `AEEIDisplay.h`'s `SetClipRect(po,
// pRect)` at that exact slot index precisely. This field is the current
// app's `IDisplay` pointer. `SetDisplayInstance()` supplies it.
//
// A fourth slot, offset 0xb0 (4 real call sites, far rarer than 0xc0),
// is GETUPTIMEMS: called with no argument, twice, around a chunk of
// per-tick work, with `second_result - first_result` used immediately
// after as an elapsed-time delta (`sub r1, r0, r7` in the real
// disassembly of Double Dragon's timer-tick callback, TASKS.md Phase
// 8) -- the standard real BREW pattern for measuring elapsed
// milliseconds around a piece of work. `Tick()` advances the millisecond
// counter this slot returns.
//
// A fifth slot, offset 0x4 (the very first slot after the table's own
// base, and one of the most frequently hit), is MEMSET: real
// disassembly of a real call site (`ddragonz.mod` offset 0x1f9b0) shows
// exactly `memset(r4+46, 0, 10)` -- r0=dest, r1=0 (the fill value),
// r2=10 (the byte count), matching ANSI `void *memset(void*, int,
// size_t)` precisely. Real ROPI-compiled code apparently routes even
// plain C library calls through this table, not just AEE/BREW-specific
// ones.
//
// A sixth slot, offset 0x14, is STRLEN: a real call site
// (`ddragonz.mod` offset 0x23b00) calls it with one argument (a string
// pointer) and immediately does `add r1, r0, #1` on the return value --
// the classic `strlen(s) + 1` idiom for computing a buffer size
// including the null terminator, matching ANSI `size_t strlen(const
// char *s)` exactly.
//
// Only these six table slots are confirmed by real disassembly so
// far. Every other offset is left unmapped -- a real .mod hitting one
// would fetch from unwritten memory, which tools/game_probe.cpp's
// wandered-outside-module check exists specifically to catch and report
// loudly rather than silently misbehave.
class ModRuntime {
 public:
  // `heap_region`/`heap_size` bound a simple bump allocator the MALLOC
  // slot hands memory out from. FREE is a no-op (leaks) -- consistent
  // with having no free-list, which is fine for a single game-session
  // emulator run. `context_address` is where the small "app context"
  // struct the offset-0xc0 slot returns is stored; must not overlap the
  // module, the heap region, or any other memory region in use.
  ModRuntime(Memory& memory, HleRuntime& hle, uint32_t heap_region, uint32_t heap_size,
             uint32_t context_address);

  // Sets the IShell object pointer the offset-0xc0 "get app context"
  // slot should expose at the confirmed field offset (+12). Safe to call
  // before or after Install().
  void SetShellInstance(uint32_t shell_ptr);

  // Sets the IDisplay object pointer the offset-0xc0 "get app context"
  // slot should expose at the confirmed field offset (+20). Safe to call
  // before or after Install().
  void SetDisplayInstance(uint32_t display_ptr);

  // Advances the millisecond counter the offset-0xb0 GETUPTIMEMS slot
  // returns. Deterministic and tick-driven (not a real wall-clock read)
  // to match how the rest of the emulator's timing works (see
  // IShellHle::Tick).
  void Tick(uint32_t elapsed_ms);

  // Writes `table_address` at `module_base - 4` and populates the
  // table's known slots. Must be called after the module itself has
  // been loaded at `module_base`; `table_address` must not overlap the
  // module or any other memory region in use.
  void Install(uint32_t module_base, uint32_t table_address);

 private:
  void MallocImpl(IArmCore& core);
  void MemsetImpl(IArmCore& core);
  void StrlenImpl(IArmCore& core);
  void GetAppContextImpl(IArmCore& core);
  void GetUpTimeMsImpl(IArmCore& core);

  Memory& memory_;
  HleRuntime& hle_;
  uint32_t heap_cursor_;
  uint32_t heap_end_;
  uint32_t context_address_;
  uint32_t shell_ptr_ = 0;
  uint32_t display_ptr_ = 0;
  uint32_t uptime_ms_ = 0;
};

}  // namespace zeebulator
