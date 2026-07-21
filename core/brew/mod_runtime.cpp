#include "core/brew/mod_runtime.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace zeebulator {

namespace {
// Offsets within the static-base table where real disassembly (see
// mod_runtime.h) shows the MALLOC/FREE/GetAppContext function pointers
// living.
constexpr uint32_t kMemcpySlotOffset = 0x0;
constexpr uint32_t kMemsetSlotOffset = 0x4;
constexpr uint32_t kStrcpySlotOffset = 0x8;
constexpr uint32_t kStrlenSlotOffset = 0x14;
constexpr uint32_t kBoundedStrcpySlotOffset = 0xe4;
constexpr uint32_t kStrstrSlotOffset = 0xe8;
constexpr uint32_t kSprintfSlotOffset = 0x13c;
constexpr uint32_t kMallocSlotOffset = 0x68;
constexpr uint32_t kFreeSlotOffset = 0x6c;
constexpr uint32_t kGetUpTimeMsSlotOffset = 0xb0;
constexpr uint32_t kGetAppContextSlotOffset = 0xc0;
constexpr uint32_t kDbgPrintfSlotOffset = 0x9c;
constexpr uint32_t kMemcpyAliasSlotOffset = 0x44;
constexpr uint32_t kReallocSlotOffset = 0x74;
constexpr uint32_t kUnknownSlotOffset0x40 = 0x40;
constexpr uint32_t kUnknownSlotOffset0xc = 0xc;
constexpr uint32_t kUnknownSlotOffset0xd0 = 0xd0;
constexpr uint32_t kUnknownSlotOffset0x184 = 0x184;
// Offsets within the "app context" struct GetAppContext returns where
// real call sites read the current app's IShell/IDisplay pointers.
constexpr uint32_t kAppContextShellOffset = 12;
constexpr uint32_t kAppContextDisplayOffset = 20;
constexpr uint32_t kAppContextThirdObjectOffset = 0x2c;
constexpr uint32_t kAppContextFourthObjectOffset = 0x24;
constexpr uint32_t kAppContextFifthObjectOffset = 0x28;
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

void ModRuntime::SetThirdContextObject(uint32_t object_ptr) { third_context_object_ = object_ptr; }

void ModRuntime::SetFourthContextObject(uint32_t object_ptr) { fourth_context_object_ = object_ptr; }

void ModRuntime::SetFifthContextObject(uint32_t object_ptr) { fifth_context_object_ = object_ptr; }

uint32_t ModRuntime::Allocate(uint32_t size) {
  uint32_t aligned = (size + 3) & ~3u;  // word-align every allocation
  if (aligned > heap_end_ - heap_cursor_) {
    return 0;  // NULL: out of (emulated) heap space
  }
  uint32_t result = heap_cursor_;
  heap_cursor_ += aligned;
  return result;
}

void ModRuntime::MallocImpl(IArmCore& core) {
  core.SetRegister(kR0, Allocate(core.GetRegister(kR0)));
}

void ModRuntime::ReallocImpl(IArmCore& core) {
  // void *realloc(void *ptr, size_t size)
  //
  // Real disassembly of Peggle (TASKS.md Phase 8, PHASE8_LOG.md) shows
  // two independent real growable-array implementations (56-byte and
  // 4-byte elements) calling this static-base slot identically:
  // `(old_ptr=array's current buffer, new_size=new_element_count *
  // element_size)`, checking the result for non-null before overwriting
  // their own buffer pointer -- exactly realloc's real contract,
  // including "leave the old block alone on failure" (this never writes
  // to `old_ptr`, only reads from it).
  //
  // This allocator has no free-list (see Allocate()/FREE's own doc
  // comment), so unlike real realloc there's no way to know the old
  // block's real size to copy just that many bytes -- copies `size`
  // bytes from the old block instead. Safe in this bump-allocator's
  // specific case even when that overreads past the old block's real
  // content: bump-allocated memory is never reused, so anything past
  // the old content is either still-zeroed, never-touched memory, or a
  // later allocation that hasn't happened yet -- and the real callers
  // observed always immediately overwrite that tail with new elements
  // right after a successful grow anyway.
  uint32_t old_ptr = core.GetRegister(kR0);
  uint32_t size = core.GetRegister(kR1);
  uint32_t new_ptr = Allocate(size);
  if (new_ptr != 0 && old_ptr != 0) {
    for (uint32_t i = 0; i < size; ++i) {
      memory_.Write8(new_ptr + i, memory_.Read8(old_ptr + i));
    }
  }
  core.SetRegister(kR0, new_ptr);
}

void ModRuntime::MemcpyImpl(IArmCore& core) {
  // void *memcpy(void *dest, const void *src, size_t n)
  uint32_t dest = core.GetRegister(kR0);
  uint32_t src = core.GetRegister(kR1);
  uint32_t count = core.GetRegister(kR2);
  for (uint32_t i = 0; i < count; ++i) {
    memory_.Write8(dest + i, memory_.Read8(src + i));
  }
  core.SetRegister(kR0, dest);  // memcpy returns its first argument
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

void ModRuntime::StrcpyImpl(IArmCore& core) {
  // char *strcpy(char *dest, const char *src)
  uint32_t dest = core.GetRegister(kR0);
  uint32_t src = core.GetRegister(kR1);
  uint32_t i = 0;
  for (;;) {
    uint8_t byte = memory_.Read8(src + i);
    memory_.Write8(dest + i, byte);
    if (byte == 0) break;
    ++i;
  }
  core.SetRegister(kR0, dest);  // strcpy returns its first argument
}

void ModRuntime::BoundedStrcpyImpl(IArmCore& core) {
  // Copy semantics inferred from the calling convention (see
  // mod_runtime.h) rather than matched to a named reference function:
  // n = min(n, cap); memcpy(dest, src, n).
  uint32_t src = core.GetRegister(kR0);
  uint32_t n = core.GetRegister(kR1);
  uint32_t dest = core.GetRegister(kR2);
  uint32_t cap = core.GetRegister(kR3);
  n = std::min(n, cap);
  for (uint32_t i = 0; i < n; ++i) {
    memory_.Write8(dest + i, memory_.Read8(src + i));
  }
  core.SetRegister(kR0, dest);
}

void ModRuntime::StrstrImpl(IArmCore& core) {
  // char *strstr(const char *haystack, const char *needle)
  uint32_t haystack = core.GetRegister(kR0);
  uint32_t needle = core.GetRegister(kR1);
  uint32_t needle_len = 0;
  while (memory_.Read8(needle + needle_len) != 0) ++needle_len;
  if (needle_len == 0) {
    core.SetRegister(kR0, haystack);
    return;
  }
  for (uint32_t i = 0;; ++i) {
    if (memory_.Read8(haystack + i) == 0) {
      core.SetRegister(kR0, 0);
      return;
    }
    bool match = true;
    for (uint32_t j = 0; j < needle_len; ++j) {
      if (memory_.Read8(haystack + i + j) != memory_.Read8(needle + j)) {
        match = false;
        break;
      }
    }
    if (match) {
      core.SetRegister(kR0, haystack + i);
      return;
    }
  }
}

void ModRuntime::SprintfImpl(IArmCore& core) {
  // Real signature and behavior inferred from the calling convention
  // plus real string content (PHASE8_LOG.md): a real call site
  // (`ddragonz.mod` offset 0x23d0c) formats the real literal string
  // "ERROR CODE:%d" (found directly in the file's own bytes at the
  // literal's address) into a stack buffer, immediately measured with
  // STRLEN afterward -- a sprintf-family helper. Signature: int
  // Func(char *dest, const char *fmt, void **ppArgs) -- ppArgs is a
  // pointer to an args cursor that gets ADVANCED by 4 bytes (this ABI's
  // word size) per consumed argument, not a plain va_list, matching the
  // double indirection at the call site (R2 points at a stack slot that
  // itself holds the args block's address). Supports the directives
  // real game code has been observed needing so far: %d, %u, %x/%X,
  // %s, %c, %% -- no width/precision/flag support (no evidence any real
  // call needs it yet; extend if one does). Returns the number of
  // characters written, matching the real sprintf() contract.
  uint32_t dest = core.GetRegister(kR0);
  uint32_t fmt = core.GetRegister(kR1);
  uint32_t args_cursor_addr = core.GetRegister(kR2);
  uint32_t args = memory_.Read32(args_cursor_addr);

  uint32_t out = dest;
  for (uint32_t i = 0;; ++i) {
    uint8_t c = memory_.Read8(fmt + i);
    if (c == 0) break;
    if (c != '%') {
      memory_.Write8(out++, c);
      continue;
    }
    uint8_t spec = memory_.Read8(fmt + i + 1);
    if (spec == 0) break;  // trailing '%' with nothing after: stop
    ++i;
    if (spec == '%') {
      memory_.Write8(out++, '%');
      continue;
    }
    std::string formatted;
    switch (spec) {
      case 'd':
        formatted = std::to_string(static_cast<int32_t>(memory_.Read32(args)));
        args += 4;
        break;
      case 'u':
        formatted = std::to_string(memory_.Read32(args));
        args += 4;
        break;
      case 'x':
      case 'X': {
        char buf[9];
        std::snprintf(buf, sizeof(buf), spec == 'x' ? "%x" : "%X", memory_.Read32(args));
        formatted = buf;
        args += 4;
        break;
      }
      case 'c':
        formatted = std::string(1, static_cast<char>(memory_.Read32(args)));
        args += 4;
        break;
      case 's': {
        uint32_t str_ptr = memory_.Read32(args);
        args += 4;
        for (uint32_t j = 0; memory_.Read8(str_ptr + j) != 0; ++j) {
          formatted.push_back(static_cast<char>(memory_.Read8(str_ptr + j)));
        }
        break;
      }
      default:
        // Unknown directive: emit literally rather than silently
        // dropping it (both the '%' and the spec character).
        formatted.push_back('%');
        formatted.push_back(static_cast<char>(spec));
        break;
    }
    for (char ch : formatted) memory_.Write8(out++, static_cast<uint8_t>(ch));
  }
  memory_.Write8(out, 0);
  memory_.Write32(args_cursor_addr, args);
  core.SetRegister(kR0, out - dest);
}

void ModRuntime::GetAppContextImpl(IArmCore& core) {
  // Written fresh on every call rather than once in Install() so this
  // works regardless of whether SetShellInstance() is called before or
  // after Install().
  memory_.Write32(context_address_ + kAppContextShellOffset, shell_ptr_);
  memory_.Write32(context_address_ + kAppContextDisplayOffset, display_ptr_);
  memory_.Write32(context_address_ + kAppContextThirdObjectOffset, third_context_object_);
  memory_.Write32(context_address_ + kAppContextFourthObjectOffset, fourth_context_object_);
  memory_.Write32(context_address_ + kAppContextFifthObjectOffset, fifth_context_object_);
  core.SetRegister(kR0, context_address_);
}

void ModRuntime::GetUpTimeMsImpl(IArmCore& core) {
  core.SetRegister(kR0, uptime_ms_);
  // Real code that busy-waits on elapsed time (confirmed by real
  // disassembly -- see TASKS.md Phase 8, Super BurgerTime's real
  // ROM-readiness poll at static-base slot 0x184) calls this in a tight
  // loop entirely within a single native HLE call, with no opportunity
  // for the outer per-frame Tick() below to ever run in between. A
  // clock that only Tick() can move would stay frozen for the loop's
  // entire lifetime, so any such real busy-wait can never see its own
  // deadline pass and would spin forever -- not a real device's
  // behavior, just an artifact of our clock only being driven
  // externally. Self-advancing by a small synthetic amount on every
  // read keeps this fully deterministic (same call sequence always
  // produces the same values, unlike a genuine wall-clock read would)
  // while still letting real elapsed-time busy-waits make forward
  // progress and eventually resolve, the same way they would on real
  // hardware given enough real wall-clock time. The 1ms-per-read rate
  // is an inferred, not measured, choice -- plausible for a real
  // "checked once per real hardware poll iteration" loop.
  uptime_ms_ += 1;
}

void ModRuntime::Tick(uint32_t elapsed_ms) { uptime_ms_ += elapsed_ms; }

void ModRuntime::Install(uint32_t module_base, uint32_t table_address) {
  uint32_t memcpy_fn = hle_.Register([this](IArmCore& core) { MemcpyImpl(core); });
  uint32_t memset_fn = hle_.Register([this](IArmCore& core) { MemsetImpl(core); });
  uint32_t strlen_fn = hle_.Register([this](IArmCore& core) { StrlenImpl(core); });
  uint32_t strcpy_fn = hle_.Register([this](IArmCore& core) { StrcpyImpl(core); });
  uint32_t malloc_fn = hle_.Register([this](IArmCore& core) { MallocImpl(core); });
  uint32_t free_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t get_uptime_ms_fn = hle_.Register([this](IArmCore& core) { GetUpTimeMsImpl(core); });
  uint32_t get_app_context_fn = hle_.Register([this](IArmCore& core) { GetAppContextImpl(core); });
  uint32_t bounded_strcpy_fn = hle_.Register([this](IArmCore& core) { BoundedStrcpyImpl(core); });
  uint32_t strstr_fn = hle_.Register([this](IArmCore& core) { StrstrImpl(core); });
  uint32_t sprintf_fn = hle_.Register([this](IArmCore& core) { SprintfImpl(core); });
  uint32_t dbgprintf_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t realloc_fn = hle_.Register([this](IArmCore& core) { ReallocImpl(core); });
  uint32_t unknown_0x40_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0xc_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0xd0_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  uint32_t unknown_0x184_fn = hle_.Register([](IArmCore& core) { core.SetRegister(kR0, 0); });
  memory_.Write32(table_address + kMemcpySlotOffset, memcpy_fn);
  memory_.Write32(table_address + kMemcpyAliasSlotOffset, memcpy_fn);
  memory_.Write32(table_address + kMemsetSlotOffset, memset_fn);
  memory_.Write32(table_address + kStrlenSlotOffset, strlen_fn);
  memory_.Write32(table_address + kStrcpySlotOffset, strcpy_fn);
  memory_.Write32(table_address + kBoundedStrcpySlotOffset, bounded_strcpy_fn);
  memory_.Write32(table_address + kStrstrSlotOffset, strstr_fn);
  memory_.Write32(table_address + kSprintfSlotOffset, sprintf_fn);
  memory_.Write32(table_address + kMallocSlotOffset, malloc_fn);
  memory_.Write32(table_address + kFreeSlotOffset, free_fn);
  memory_.Write32(table_address + kGetUpTimeMsSlotOffset, get_uptime_ms_fn);
  memory_.Write32(table_address + kGetAppContextSlotOffset, get_app_context_fn);
  memory_.Write32(table_address + kDbgPrintfSlotOffset, dbgprintf_fn);
  memory_.Write32(table_address + kReallocSlotOffset, realloc_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x40, unknown_0x40_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0xc, unknown_0xc_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0xd0, unknown_0xd0_fn);
  memory_.Write32(table_address + kUnknownSlotOffset0x184, unknown_0x184_fn);
  memory_.Write32(module_base - 4, table_address);
}

}  // namespace zeebulator
