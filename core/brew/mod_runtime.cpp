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
  core.SetRegister(kR0, context_address_);
}

void ModRuntime::GetUpTimeMsImpl(IArmCore& core) { core.SetRegister(kR0, uptime_ms_); }

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
  memory_.Write32(module_base - 4, table_address);
}

}  // namespace zeebulator
