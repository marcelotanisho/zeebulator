// Dev tool: drives a real BREW game's full lifecycle (AEEMod_Load ->
// IModule::CreateInstance -> HandleEvent(EVT_APP_START)) through every
// HLE interface implemented so far (IShell, IDisplay, IFile/IFileMgr,
// IGL/IEGL, IMedia), to see how far real execution actually gets and
// exactly which gap it hits next -- the "iteratively debug against the
// real game" approach documented in PHASE8_LOG.md. Real-file validation
// only: takes paths at runtime, never embeds/bundles game content (see
// CONTRIBUTING.md's clean-room policy).

#include <SDL.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

#include "core/audio/mixer.h"
#include "core/brew/file_hle.h"
#include "core/brew/gl_hle.h"
#include "core/brew/idisplay.h"
#include "core/brew/interface_object.h"
#include "core/brew/ishell.h"
#include "core/brew/media_hle.h"
#include "core/brew/mod_runtime.h"
#include "core/brew/scaffold_object.h"
#include "core/brew/virtual_filesystem.h"
#include "core/cpu/arm_interpreter.h"
#include "core/loader/ggz.h"
#include "core/loader/mod.h"
#include "frontends/standalone/sdl2_backend.h"
#include "frontends/standalone/sdl2_gl_backend.h"

namespace {

std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: couldn't open '%s'\n", path);
    std::exit(1);
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
}

// Real disassembly of Double Dragon (PHASE8_LOG.md) shows it calling
// IFILEMGR_OpenFile("sound.ggz") directly -- the game opens its own
// packed resource archive as a raw file and presumably streams/parses
// it itself (e.g. for on-demand audio), rather than expecting every
// entry pre-extracted. So the archive's own raw bytes need to be a
// VFS entry under its basename too, alongside its extracted contents.
std::string BaseName(const char* path) {
  std::string s(path);
  size_t slash = s.find_last_of("/\\");
  return slash == std::string::npos ? s : s.substr(slash + 1);
}

void MergeGgzInto(zeebulator::VirtualFilesystem& vfs, const char* path) {
  std::vector<uint8_t> raw = ReadFile(path);
  auto archive = zeebulator::GgzArchive::Parse(raw);
  for (const auto& entry : archive.Entries()) {
    vfs.AddFile(entry.name, archive.Extract(entry));
  }

  // Double Dragon's own real code, when it opens "sound.ggz" itself
  // (rather than going through this loop's per-entry extraction above),
  // reads each entry's declared `decompressed_size` as a literal raw
  // byte count straight from the file at `offset` -- no decompression
  // at that level (confirmed via direct disassembly of ddragonz.mod
  // 0x11bfd0/0x11c964, PHASE8_LOG.md) -- so it needs the *raw file* to
  // physically contain that many bytes, not just a valid gzip stream
  // that happens to decompress to that size. This repo's `sound.ggz`
  // (byte-identical across three independent public sources) is short
  // for its own last few entries -- e.g. entry 73 declares 1034 bytes
  // at offset 1927592, but the file ends 529 bytes early. A real,
  // independent Zeebo emulator (Infuse) plays Double Dragon successfully
  // against this same file, which only makes sense if it tolerates this
  // exact shortfall -- so this pads the raw copy exposed under the
  // archive's own basename (never the individually-extracted, correctly
  // decompressed entries above) with zero bytes out to the largest
  // offset+decompressed_size any entry declares. This does not
  // fabricate any real content (the genuinely missing tail of that one
  // background track stays silent/garbage padding, not guessed audio)
  // -- it only stops a short real file from producing a false EOF where
  // a real, correct player evidently doesn't hit one.
  uint32_t max_extent = static_cast<uint32_t>(raw.size());
  for (const auto& entry : archive.Entries()) {
    uint32_t extent = entry.offset + entry.decompressed_size;
    if (extent > max_extent) max_extent = extent;
  }
  if (max_extent > raw.size()) {
    std::printf("padding %s with %zu zero bytes (short by that much vs. its own header table)\n",
                path, static_cast<size_t>(max_extent) - raw.size());
    raw.resize(max_extent, 0);
  }

  vfs.AddFile(BaseName(path), std::move(raw));
  std::printf("loaded %zu entries from %s\n", archive.Entries().size(), path);
}

// Like HleRuntime::CallArmFunction, but bounded and loudly reports if
// execution ever fetches from outside the loaded module's own address
// range (and outside the HLE call-out trap range). Real game code CAN
// legitimately jump outside the module briefly (into an HLE call-out),
// but a fetch from anywhere else -- e.g. never-written/zero-filled
// memory -- is never real progress. Without this check, our interpreter
// silently decodes an all-zero word as a harmless "ANDEQ r0,r0,r0" and
// keeps going; concretely, this is exactly what happened probing Double
// Dragon's real AEEMod_Load (see PHASE8_LOG.md): a missing loader
// "static base" pointer caused an indirect call through a null function
// pointer to jump to address 0, and stepping through zeroed memory from
// there happened to walk (262,237 harmless no-op steps later) right back
// into the module's own base address, silently re-entering AEEMod_Load
// and eventually producing a coincidentally-truthy but meaningless
// "success" result -- not a crash, not an UnimplementedInstruction, just
// quietly wrong. This helper turns that into a loud, unmissable warning
// instead.
struct CallResult {
  uint32_t r0 = 0;
  bool wandered_outside_module = false;
  bool exceeded_step_budget = false;
};

CallResult CallArmFunctionChecked(zeebulator::ArmInterpreter& cpu, uint32_t trap_base,
                                   uint32_t mod_base, uint32_t mod_size, uint32_t entry,
                                   uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                                   bool trace = false, bool hle_trace = false) {
  constexpr uint64_t kMaxSteps = 5'000'000;
  cpu.SetRegister(zeebulator::kR0, r0);
  cpu.SetRegister(zeebulator::kR1, r1);
  cpu.SetRegister(zeebulator::kR2, r2);
  cpu.SetRegister(zeebulator::kR3, r3);
  cpu.SetRegister(zeebulator::kLR, trap_base);
  cpu.SetRegister(zeebulator::kPC, entry);

  CallResult result;
  uint32_t last_in_module_pc = 0;
  uint32_t last_lr = 0;
  for (uint64_t steps = 0; cpu.GetRegister(zeebulator::kPC) != trap_base; ++steps) {
    if (steps >= kMaxSteps) {
      std::printf("warning: exceeded %llu steps without returning -- aborting this call\n",
                  static_cast<unsigned long long>(kMaxSteps));
      result.exceeded_step_budget = true;
      break;
    }
    uint32_t pc = cpu.GetRegister(zeebulator::kPC);
    bool in_module = pc >= mod_base && pc < mod_base + mod_size;
    bool in_trap_range = pc >= trap_base;
    if (in_module) {
      last_in_module_pc = pc;
      last_lr = cpu.GetRegister(zeebulator::kLR);
    }
    if (trace) {
      std::printf("[%4llu] pc=0x%08x instr=0x%08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x\n",
                  static_cast<unsigned long long>(steps), pc, cpu.GetMemory().Read32(pc),
                  cpu.GetRegister(zeebulator::kR0), cpu.GetRegister(zeebulator::kR1),
                  cpu.GetRegister(zeebulator::kR2), cpu.GetRegister(zeebulator::kR3),
                  cpu.GetRegister(zeebulator::kR4));
    }
    if (hle_trace && in_trap_range && pc != trap_base) {
      std::printf("  [hle call] trap=0x%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", pc,
                  cpu.GetRegister(zeebulator::kR0), cpu.GetRegister(zeebulator::kR1),
                  cpu.GetRegister(zeebulator::kR2), cpu.GetRegister(zeebulator::kR3));
    }
    if (!in_module && !in_trap_range && !result.wandered_outside_module) {
      std::printf(
          "warning: pc=0x%08x left the loaded module's range (0x%08x-0x%08x) after %llu "
          "steps -- likely a missing loader/runtime-support gap, not real progress (see "
          "PHASE8_LOG.md). Last in-module pc=0x%08x lr=0x%08x -- disassemble there first.\n",
          pc, mod_base, mod_base + mod_size, static_cast<unsigned long long>(steps),
          last_in_module_pc, last_lr);
      result.wandered_outside_module = true;  // only warn once per call
    }
    cpu.Step();
  }
  result.r0 = cpu.GetRegister(zeebulator::kR0);
  return result;
}

// Maps a subset of SDL keys to real BREW AVK-family key codes for
// exploratory input testing. The exact AVK_* enum values aren't
// confirmed against a real header this session -- what IS confirmed via
// real disassembly (PHASE8_LOG.md) is that Double Dragon's own
// HandleEvent treats wParam values in [0xe021, 0xe021+22] as key codes,
// converting them to a bitmask via a jump table. This maps number keys
// 0-9 to that range's first 10 offsets (0xe021..0xe02a) and arrow keys
// to the next four (0xe02b..0xe02e), purely so real keypresses can be
// tried against the running game and their effect (if any) observed --
// not a claimed-correct real key mapping.
uint32_t SdlKeyToAvk(SDL_Keycode key) {
  constexpr uint32_t kAvkBase = 0xe021;
  if (key >= SDLK_0 && key <= SDLK_9) {
    return kAvkBase + static_cast<uint32_t>(key - SDLK_0);
  }
  switch (key) {
    case SDLK_UP: return kAvkBase + 10;
    case SDLK_DOWN: return kAvkBase + 11;
    case SDLK_LEFT: return kAvkBase + 12;
    case SDLK_RIGHT: return kAvkBase + 13;
    default: return 0;
  }
}

// EXPERIMENTAL: a fake connected-joystick handle, reported by the HID
// scaffold below instead of the honest "zero devices" answer this
// project used through TASKS.md Phase 8's Double Dragon investigation.
// Found (real disassembly, see PHASE8_LOG.md) that Double Dragon's
// title screen genuinely, correctly waits for real HID/gamepad input
// before proceeding -- not an emulator bug, a real hardware dependency
// this dev tool has no real controller to satisfy. Any nonzero, stable
// value works as the "handle" -- real code only ever uses it as an
// opaque token passed back into IHID_CreateDevice/GetDeviceInfo, never
// interprets it directly.
constexpr uint32_t kSimulatedDeviceHandle = 1;

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    // cls_id is IModule::CreateInstance's real AEECLSID -- the literal
    // the module's own code compares the passed ClsId against (found by
    // tracing the first few real instructions of CreateInstance with
    // trace=true; it's loaded via a PC-relative `ldr` right before the
    // `cmp` that decides success/failure). NOT necessarily the game's
    // download-catalog folder number: confirmed identical to it for
    // Super BurgerTime (279125), but genuinely different for Double
    // Dragon (274754 vs the real 0x0102f789) and Peggle (278962 vs the
    // real 0x01099cd6) -- passing the folder number for those two makes
    // CreateInstance return EFAILED immediately, with zero HLE calls,
    // before anything resembling real progress happens. See
    // PHASE8_LOG.md for how this was found.
    std::fprintf(stderr, "usage: %s <game.mod> <data.ggz> <sound.ggz> <cls_id_decimal>\n",
                  argv[0]);
    return 1;
  }
  auto mod_data = ReadFile(argv[1]);
  auto cls_id = static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10));

  zeebulator::VirtualFilesystem vfs;
  MergeGgzInto(vfs, argv[2]);
  MergeGgzInto(vfs, argv[3]);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  constexpr int kWidth = 640;
  constexpr int kHeight = 480;
  constexpr int kAudioSampleRate = 22050;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  SDL_Window* window =
      SDL_CreateWindow("Zeebulator - game probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                        kWidth, kHeight, SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL);
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

  zeebulator::ArmInterpreter cpu;
  constexpr uint32_t kTrapBase = 0xF0000000;
  zeebulator::HleRuntime hle(cpu, kTrapBase, 0x10000);
  zeebulator::Sdl2Backend backend(renderer, kWidth, kHeight, kAudioSampleRate);
  zeebulator::IDisplayHle display(backend, kWidth, kHeight);
  zeebulator::Sdl2GlBackend gl_backend(window);
  zeebulator::GlHle gl_hle(gl_backend);
  zeebulator::Mixer mixer(kAudioSampleRate);
  zeebulator::FileHle file_hle(cpu.GetMemory(), hle, vfs, /*object_region=*/0x80100000);
  zeebulator::MediaHle media_hle(cpu.GetMemory(), hle, vfs, mixer, /*object_region=*/0x80200000);

  constexpr uint32_t kBase = 0x00100000;
  zeebulator::LoadMod(cpu, mod_data, kBase);
  auto mod_size = static_cast<uint32_t>(mod_data.size());

  // Real compiled .mod code (ARM RVCT ROPI convention) expects a
  // "static base" pointer at kBase-4 -- see core/brew/mod_runtime.h and
  // PHASE8_LOG.md for how this was found via real disassembly.
  // heap_size was originally 1 MiB, sized arbitrarily rather than
  // measured -- real disassembly of Double Dragon's own resource-list
  // loader (PHASE8_LOG.md) shows it MALLOC-ing real, sizeable audio
  // buffers (tens to hundreds of KB each) for many real resources in a
  // row, genuinely exhausting 1 MiB partway through and returning a
  // real null from MALLOC that real game code can't recover from --
  // not a bug in MallocImpl itself (confirmed via a live debug trace),
  // just too small a heap for this real game's real needs. Bumped to
  // 16 MiB, a generous but not unreasonable amount of app heap for a
  // 2009-era dedicated gaming device.
  zeebulator::ModRuntime mod_runtime(cpu.GetMemory(), hle, /*heap_region=*/0x80300000,
                                      /*heap_size=*/0x01000000, /*context_address=*/0x80280200);
  mod_runtime.Install(kBase, /*table_address=*/0x80280000);

  uint32_t display_obj =
      display.Build(cpu.GetMemory(), hle, /*vtable=*/0x80002000, /*object=*/0x80003000);
  // Real compiled app code obtains IDisplay through
  // ISHELL_CreateInstance(AEECLSID_DISPLAY, ...), not directly -- found
  // via real disassembly of AEEApplet_New's call chain (PHASE8_LOG.md).
  zeebulator::IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.RegisterInstance(/*AEECLSID_DISPLAY=*/0x01001001, display_obj);
  // ClsId 0x01002001: a real BREW class Double Dragon's own graphics-init
  // routine requires (ISHELL_CreateInstance failing for it is the
  // confirmed root cause of the "memory insufficient" dead end -- see
  // PHASE8_LOG.md). Its real interface isn't identified yet, so this
  // is a generic scaffold (see scaffold_object.h) sized to cover the
  // highest slot (33) real disassembly shows the game calling on it.
  uint32_t unknown_graphics_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8000C000, /*object=*/0x8000D000, /*slot_count=*/40);
  shell_hle.RegisterInstance(/*unidentified, real ClsId from disassembly=*/0x01002001,
                              unknown_graphics_obj);
  uint32_t shell = shell_hle.Build(/*vtable=*/0x80000000, /*object=*/0x80001000);
  // Same real init routine also calls IDisplay::GetDeviceBitmap and
  // immediately dereferences the result's vtable -- another generic
  // scaffold, since the real IBitmap-shaped interface isn't identified
  // either. Real disassembly of a second, deeper call site (0x1d5b8)
  // shows the returned bitmap's slot 2 gets called in a
  // "QueryInterface"-shaped way (obj, clsid=0x01001045, &ppo) and the
  // result immediately Release()'d if null -- so unlike the other
  // scaffolds so far, this one slot needs a real (if still generic)
  // implementation, not a blind Stub. clsid=0x01001045 is very likely
  // real AEECLSID_DIB: a real bundled BREW OGLES sample
  // (simple_drawtexture.c, under research/docs/sdk_installer_extract/
  // ZeeboSDKPackage-1.2.4/samples.zip) does exactly this same call --
  // `IBITMAP_QueryInterface(pIBitmapDDB, AEECLSID_DIB, (void**)&pDIB)`
  // right after `IDISPLAY_GetDeviceBitmap` -- then casts the result
  // straight to `NativeWindowType` for `eglCreateWindowSurface`, which
  // is exactly how the scaffold below gets used one call site over.
  // The numeric ClsId itself isn't in any bundled header (only the
  // matching call shape + matching downstream use), so this is strong
  // circumstantial evidence, not a confirmed literal match like
  // AEECLSID_GL/EGL/HID above -- doesn't change any behavior either way
  // since the scaffold is generic regardless of the class's real name.
  uint32_t unknown_0x01001045_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80018000, /*object=*/0x80019000, /*slot_count=*/20);
  uint32_t device_bitmap_obj = zeebulator::BuildStubObjectWithOverride(
      cpu.GetMemory(), hle, /*vtable=*/0x8000E000, /*object=*/0x8000F000, /*slot_count=*/20,
      /*override_slot=*/2,
      [&cpu, unknown_0x01001045_obj](zeebulator::IArmCore& core) {
        uint32_t requested_cls = core.GetRegister(zeebulator::kR1);
        uint32_t ppo = core.GetRegister(zeebulator::kR2);
        if (requested_cls == 0x01001045) {
          cpu.GetMemory().Write32(ppo, unknown_0x01001045_obj);
          core.SetRegister(zeebulator::kR0, 0);
        } else {
          core.SetRegister(zeebulator::kR0, 1);
        }
      });
  display.SetDeviceBitmapInstance(device_bitmap_obj);
  // ClsId 0x01001003: real disassembly of 0x1b2fc showed
  // ISHELL_CreateInstance gating the same "memory insufficient" state on
  // this class alongside 0x01001014 below -- initially scaffolded
  // generically since neither is dereferenced within 0x1b2fc itself.
  // Deeper disassembly (0x1c6b0 -> 0x22384 -> 0x237c4 -> 0x9f3c, TASKS.md
  // Phase 8) later showed this object IS used, extensively, by the
  // applet's own save-game load/create routine: IFILEMGR_Test on
  // "./udata/ddz.sav", IFILEMGR_GetFreeSpace checked against a minimum,
  // then IFILEMGR_OpenFile with mode literal 2 -- which is exactly real
  // AEEFile.h's _OFM_READWRITE (also confirmed a literal 4 elsewhere in
  // the same routine, matching _OFM_CREATE). That's IFileMgr's real
  // vtable shape exactly (slot 2 OpenFile, slot 7 Test, slot 8
  // GetFreeSpace) -- so this is very likely real AEECLSID_FILEMGR
  // (not confirmed by a literal number match, unlike AEECLSID_GL/EGL/HID,
  // since no bundled header states FILEMGR's numeric ClsId -- but the
  // vtable shape and the exact "test/create-if-missing/open" flow
  // matching real AEEFile.h leave little doubt). This project's own
  // FileHle already implements real IFileMgr/IFile (built in an earlier
  // phase for GGZ-backed read-only content, extended this round with a
  // real writable "user data" store -- see file_hle.h -- specifically so
  // this save-file flow can genuinely succeed instead of merely not
  // crashing) -- wired in below instead of a generic scaffold.
  uint32_t file_mgr_obj = file_hle.Build(/*file_mgr_vtable=*/0x80004000,
                                          /*file_mgr_object=*/0x80005000,
                                          /*file_vtable=*/0x80006000);
  shell_hle.RegisterInstance(0x01001003, file_mgr_obj);
  // ClsId 0x01001014: created alongside 0x01001003 above and never
  // reassigned anywhere in the traced code (confirmed with a live
  // memory watchpoint across a full run -- see PHASE8_LOG.md). Real
  // disassembly shows its slot 3 (Read) called immediately after the
  // game's own resource-loading routine opens and seeks the real file
  // it wants via a *different* object (IFileMgr), with no attach/bind
  // step ever observed -- so a real generic scaffold's blind Stub
  // silently "succeeds" with 0 bytes read every time. Wired to
  // FileHle's last-opened-file proxy instead: an evidence-grounded
  // educated implementation of the one behavior everything points at
  // (see file_hle.h's own doc comment on BuildLastOpenedFileProxy for
  // the full reasoning and what's still unconfirmed about it).
  uint32_t last_opened_file_proxy =
      file_hle.BuildLastOpenedFileProxy(/*vtable=*/0x80012000, /*object=*/0x80013000);
  shell_hle.RegisterInstance(0x01001014, last_opened_file_proxy);
  // A still-deeper gate (0x1d5b8, reached only after the fixes above)
  // requires two more classes -- confirmed via real objdump directly on
  // the literal pool addresses its own `ldr r1,[pc,#N]` instructions
  // reference (0x1d970/0x1d974), not assumed from the nearby-looking
  // 0x0100100x classes above (a first attempt reused those by mistake
  // and was caught because the resulting crash's real disassembly showed
  // different literal values at those addresses). These turned out to be
  // real, already-implemented classes: extracted the real BREW OpenGL ES
  // extension SDK (`research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/
  // OpenGLES_Extension_...zip`, an MSI -- unpacked its embedded cabinet
  // with `7z`/`cabextract`) and found its real `AEEGL.h`: `#define
  // AEECLSID_GL 0x01014bc3` / `#define AEECLSID_EGL 0x01014bc4`, exactly
  // matching. `GlHle` (built in an earlier phase, previously wired up
  // directly without going through `CreateInstance`) already implements
  // both real interfaces, so those replace the generic scaffolds here.
  uint32_t gl_obj = gl_hle.BuildGl(cpu.GetMemory(), hle, /*vtable=*/0x80007000, /*object=*/0x80008000);
  shell_hle.RegisterInstance(/*AEECLSID_GL=*/0x01014bc3, gl_obj);
  uint32_t egl_obj = gl_hle.BuildEgl(cpu.GetMemory(), hle, /*vtable=*/0x80009000, /*object=*/0x8000A000);
  shell_hle.RegisterInstance(/*AEECLSID_EGL=*/0x01014bc4, egl_obj);
  // A still-deeper gate (0x1b71c, a joystick/gamepad-init routine gating
  // the same "memory insufficient" state) calls
  // ISHELL_CreateInstance(shell, ClsId=0x0106c411, ...) then
  // IHID_GetConnectedDevices(pIHID, AEEUID_HID_Joystick_Device, ...) --
  // both literals confirmed for real: 0x0106c411 = AEECLSID_HID and
  // 0x0106c3fd = AEEUID_HID_Joystick_Device both appear, named exactly,
  // in the real BREW SDK sample source bundled in this repo
  // (research/samples/conftest_source/conftest/GamepadMgr.c and the
  // extracted AEEIHID.h under research/docs/sdk_installer_extract/
  // sdk_installer_cab/), which also confirms GetConnectedDevices is real
  // vtable slot 7 (INHERIT_IQI's 3 slots + CreateDevice/GetDeviceInfo/
  // GetNextConnectEvent/RegisterForConnectEvents/GetConnectedDevices).
  // We have no real joystick hardware to enumerate, so honestly
  // reporting zero connected devices was the original, correct answer
  // here.
  //
  // EXPERIMENTAL escalation (TASKS.md Phase 8): reports one simulated
  // joystick instead, to see what real code does with an actually-
  // connected device -- see kSimulatedDeviceHandle's own doc comment
  // near main()'s top for why. Real disassembly of what followed
  // (`ddragonz.mod`, traced live, not guessed) showed real code
  // immediately calling real vtable slot 3,
  // `IHID_CreateDevice(pIHID, nHandle, &ppDevice)`, and -- like every
  // other unchecked-result call site in this project's history --
  // dereferencing `*ppDevice` shortly after without checking the
  // return code. A blind stub answering slot 3 (the same "return 0,
  // touch nothing else" default every other slot here still uses)
  // left `*ppDevice` null, and real code wandered into it. Slot 3 is
  // now also overridden, handing back a second, separate generic
  // scaffold object (real `IHIDDevice` shape not confirmed against any
  // header, same deliberately-unguessed treatment as this file's other
  // unknown interfaces) instead of leaving the output pointer
  // untouched. That scaffold needs 40 slots, not this file's usual
  // 10-slot default for a *known*, fully-specified small interface:
  // real disassembly (also traced live) showed real code calling
  // IHIDDevice's own vtable slot 11 (byte offset 44) next, which a
  // 10-slot object doesn't have room for -- reading past its own
  // vtable into unmapped memory decoded as a null function pointer and
  // wandered exactly the same way.
  constexpr uint32_t kHidDeviceVtable = 0x80063000;
  constexpr uint32_t kHidDeviceObject = 0x80064000;
  // Real vtable ordering confirmed directly against the bundled real
  // AEEIHIDDevice.h (research/docs/sdk_installer_extract/sdk_installer_cab):
  // INHERIT_IQI's 3 slots (AddRef/Release/QueryInterface), then
  // GetDeviceInfo/GetDeviceStatus/RegisterForStatusChange/GetButtonInfo/
  // GetNumberOfButtons/RegisterForButtonEvent(8)/GetNextButtonEvent(9)/
  // GetPositionState/GetMinPositionInfo(11)/GetMaxPositionInfo(12)/
  // GetAxesInfo(13)/... -- slots 11-13 already matched real Double Dragon
  // call sites (`ddragonz.mod` offset 0x100af4-0x100b48) exactly.
  //
  // Queue of simulated AEEHIDButtonInfo events for GetNextButtonEvent(9)
  // to hand out one at a time -- how a real button *press* gets
  // delivered to real code, once real code asks for it. Each entry is
  // {nButtonID, nState, nButtonUID}; nButtonMin/nButtonMax are always
  // 0/1 for a simple digital button per the real header's own docs.
  auto simulated_button_events =
      std::make_shared<std::vector<std::array<int32_t, 3>>>();
  std::vector<zeebulator::HleRuntime::HleFunction> hid_device_methods(
      40, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  hid_device_methods[8] = [](zeebulator::IArmCore& core) {
    // AEEResult RegisterForButtonEvent(IHIDDevice*, ISignal *piSignal)
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  hid_device_methods[9] = [simulated_button_events](zeebulator::IArmCore& core) {
    // AEEResult GetNextButtonEvent(IHIDDevice*, AEEHIDButtonInfo *pnButtonInfo,
    //   uint32 *pdwTimestamp, boolean *pbDroppedEvents)
    if (simulated_button_events->empty()) {
      core.SetRegister(zeebulator::kR0, 1);  // no more events (AEE_EFAILED-ish)
      return;
    }
    auto [button_id, state, button_uid] = simulated_button_events->front();
    simulated_button_events->erase(simulated_button_events->begin());
    uint32_t info_addr = core.GetRegister(zeebulator::kR1);
    // struct AEEHIDButtonInfo { int nButtonID; int nState; int nButtonUID;
    //   int nButtonMin; int nButtonMax; } -- confirmed field order/size
    // directly against the real AEEIHIDDevice.h.
    core.GetMemory().Write32(info_addr + 0, static_cast<uint32_t>(button_id));
    core.GetMemory().Write32(info_addr + 4, static_cast<uint32_t>(state));
    core.GetMemory().Write32(info_addr + 8, static_cast<uint32_t>(button_uid));
    core.GetMemory().Write32(info_addr + 12, 0);
    core.GetMemory().Write32(info_addr + 16, 1);
    uint32_t timestamp_addr = core.GetRegister(zeebulator::kR2);
    if (timestamp_addr != 0) core.GetMemory().Write32(timestamp_addr, 0);
    uint32_t dropped_addr = core.GetRegister(zeebulator::kR3);
    if (dropped_addr != 0) core.GetMemory().Write32(dropped_addr, 0);
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  uint32_t hid_device_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, kHidDeviceVtable, kHidDeviceObject, hid_device_methods);
  std::vector<zeebulator::HleRuntime::HleFunction> hid_methods(
      10, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  hid_methods[3] = [hid_device_obj](zeebulator::IArmCore& core) {
    // AEEResult CreateDevice(IHID*, int nHandle, IHIDDevice **ppDevice)
    uint32_t ppdevice = core.GetRegister(zeebulator::kR2);
    if (ppdevice != 0) {
      core.GetMemory().Write32(ppdevice, hid_device_obj);
    }
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  hid_methods[7] = [](zeebulator::IArmCore& core) {
    // AEEResult GetConnectedDevices(IHID*, int nDeviceType,
    //   int *pnDevHandles, int pnDevHandlesLen, int *pnDevHandlesLenReq)
    uint32_t device_handles_addr = core.GetRegister(zeebulator::kR2);
    uint32_t device_handles_len = core.GetRegister(zeebulator::kR3);
    uint32_t num_handles_req_addr = zeebulator::HleRuntime::ReadStackArg(core, 0);
    if (device_handles_addr != 0 && device_handles_len >= 1) {
      core.GetMemory().Write32(device_handles_addr, kSimulatedDeviceHandle);
    }
    if (num_handles_req_addr != 0) {
      core.GetMemory().Write32(num_handles_req_addr, 1);
    }
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  uint32_t hid_obj = zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, /*vtable_address=*/0x8001C000,
                                                       /*object_address=*/0x8001D000, hid_methods);
  shell_hle.RegisterInstance(/*AEECLSID_HID=*/0x0106c411, hid_obj);
  // A second class this same routine unconditionally requires next
  // (gated on GetConnectedDevices' own success, not on device count --
  // still reached with zero devices). Confirmed this round (not just
  // call-order-shaped anymore) to be real AEECLSID_SignalCBFactory: its
  // slot 3 is really `ISignalCBFactory_CreateSignal(this, IDLECBFUNC pfn,
  // void *pUser, ISignal **ppISignal, ISignalCtl **ppISignalCtl)` --
  // confirmed by comparing three real call sites this round (all through
  // this same slot) against the real reference implementation in
  // research/samples/conftest_source/conftest/GamepadMgr.c, which
  // registers exactly three signals in exactly this order: a device
  // connect signal, a button-event signal, then a position-change
  // signal. Real Double Dragon code takes the same shape; its real
  // button-event callback address (`ddragonz.mod` 0x11bdf4, confirmed by
  // disassembly to match `L_JoystickButtonCB`'s real shape: it reads a
  // real AEEHIDButtonInfo -- see the AEEIHIDDevice.h struct definition
  // bundled in this repo's research/ -- and updates real per-button
  // bitmasks) is captured here so a simulated button press can invoke it
  // directly later, the same way a real fired ISignal would.
  auto captured_button_callback = std::make_shared<uint32_t>(0);
  auto captured_button_context = std::make_shared<uint32_t>(0);
  constexpr uint32_t kRealButtonCallbackAddress = 0x0011bdf4;
  std::vector<zeebulator::HleRuntime::HleFunction> signal_cb_factory_methods(
      20, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  signal_cb_factory_methods[3] = [captured_button_callback,
                                   captured_button_context](zeebulator::IArmCore& core) {
    // AEEResult CreateSignal(ISignalCBFactory*, IDLECBFUNC pfn, void *pUser,
    //   ISignal **ppISignal, ISignalCtl **ppISignalCtl)
    uint32_t callback = core.GetRegister(zeebulator::kR1);
    uint32_t user_data = core.GetRegister(zeebulator::kR2);
    uint32_t out_signal_ctl = zeebulator::HleRuntime::ReadStackArg(core, 0);
    if (callback == kRealButtonCallbackAddress) {
      *captured_button_callback = callback;
      *captured_button_context = user_data;
    }
    if (out_signal_ctl != 0) {
      // Real code only ever checks this pointer for null/non-null
      // (RegisterFor*Event's own ISignal argument) and calls Detach/
      // Release on it at teardown, which this dev tool's own process
      // lifetime never reaches -- any stable nonzero token is enough.
      core.GetMemory().Write32(out_signal_ctl, 0x80065000);
    }
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  uint32_t unknown_0x01041207_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable_address=*/0x8001E000, /*object_address=*/0x8001F000,
      signal_cb_factory_methods);
  shell_hle.RegisterInstance(0x01041207, unknown_0x01041207_obj);
  // A real, previously-unreachable class found while testing the
  // simulated-connected-joystick change above (TASKS.md Phase 8):
  // once GetConnectedDevices reports a device, real code goes on to
  // call `ISHELL_CreateInstance(shell, ClsId=0x01005511, ...)` --
  // unconfirmed against any real header, but reached only through this
  // real HID-device code path, and -- like every other real
  // `CreateInstance` call site in this project's history -- not
  // checked for failure before its result gets used, wandering into
  // unmapped memory when left unregistered.
  //
  // Traced live this round (real call sites, not guessed): slot 4 gets
  // called three times with a small-integer-ID/value shape (`SetProperty`
  // -like: `(4, id=1, val_ptr)`, `(4, id=0x10, val=1)`, `(4, id=4,
  // val=0)`), then slot 3 registers a real callback (`ddragonz.mod`
  // `0x11d020`) with a real userdata pointer, then slot 6 gets one more
  // call. Disassembling the real registered callback: it only acts on an
  // event struct with field `+8 == 4` and field `+16` in `{2, 3}`,
  // branching straight into a second real function (`0x11f4dc`) that --
  // given a real sub-object at `pUser+8` and a nonzero byte at
  // `pUser+37` -- calls a real vtable slot 11 method with the literal
  // argument `100`. That shape (a status/percentage report, gated behind
  // a real download-catalog-style class ID, reached only once a real HID
  // controller was already detected -- i.e. as part of a broader real
  // "is the environment ready" sequence) strongly resembles Zeebo's own
  // real download/install-progress notification service, given the
  // platform's real download-based distribution model this whole
  // project's own catalog-ID handling already reflects. This repo's own
  // game assets genuinely are complete (three independent sources agree
  // byte-for-byte, PHASE8_LOG.md), so reporting "100% / complete" here
  // is a truthful simulation of real environment state, not a guessed
  // condition -- the same spirit as the already-simulated HID controller,
  // not a new kind of guess.
  auto captured_download_callback = std::make_shared<uint32_t>(0);
  auto captured_download_context = std::make_shared<uint32_t>(0);
  std::vector<zeebulator::HleRuntime::HleFunction> unknown_0x01005511_methods(
      20, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  unknown_0x01005511_methods[3] = [captured_download_callback,
                                    captured_download_context](zeebulator::IArmCore& core) {
    *captured_download_callback = core.GetRegister(zeebulator::kR1);
    *captured_download_context = core.GetRegister(zeebulator::kR2);
    core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
  };
  uint32_t unknown_0x01005511_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable_address=*/0x80060000, /*object_address=*/0x80061000,
      unknown_0x01005511_methods);
  shell_hle.RegisterInstance(0x01005511, unknown_0x01005511_obj);
  // Two more real, unidentified classes found investigating why
  // Peggle's tick loop settles into a fixed, non-progressing steady
  // state (TASKS.md Phase 8). The first is half of a real "try the
  // newer class, fall back to the older one" pair: real disassembly
  // (`peggle.mod` offset 0x104a50-0x104aa0) shows `ISHELL_CreateInstance`
  // called with ClsId 0x0103d8ec first, and -- only if that fails --
  // ClsId 0x01014bc4 next, both immediately after an `AddRef`-shaped
  // call on the same real IShell pointer. This exact instruction
  // sequence, both literal ClsIds included, also appears verbatim in a
  // second, independently-compiled real title
  // (`Super BurgerTime/mod/279125/supbtime.mod` offset 0x110e64-
  // 0x110ef4) -- strong evidence this is a real, standard SDK/compiler-
  // emitted helper rather than anything Peggle-specific. `0x01014bc4`
  // is NOT a second unidentified class needing its own scaffold, though
  // -- it's the already-real, already-registered `AEECLSID_EGL` above
  // (confirmed via the bundled SDK headers, not a guess). A generic
  // stub was mistakenly registered for it here too in an earlier round,
  // silently shadowing the real EGL object for every title, including
  // Double Dragon's own unrelated, direct `CreateInstance(AEECLSID_EGL)`
  // call during real graphics init -- found by tracing exactly why that
  // call's own `eglGetDisplay` was returning a blind 0 (TASKS.md Phase
  // 8). The fallback class this stub existed for is dead code anyway:
  // `0x0103d8ec` is itself an always-succeeding stub, so real code
  // never actually reaches the ClsId 0x01014bc4 fallback branch at all.
  // Only `0x0103d8ec` needs a scaffold here.
  uint32_t unknown_0x0103d8ec_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80040000, /*object=*/0x80041000, /*slot_count=*/40);
  shell_hle.RegisterInstance(0x0103d8ec, unknown_0x0103d8ec_obj);
  // The third class, ClsId 0x01030766 (`peggle.mod` offset
  // 0x10a208-0x10a24c), is reached via a real IShell pointer stored at
  // offset +12 of the function's own struct parameter -- the same
  // confirmed Shell-field convention as the ambient app context struct
  // -- with its result stored unconditionally (no failure check) into
  // that struct's own offset +0x48. Same generic, deliberately-
  // unguessed scaffold treatment already established for `0x01002001`
  // (see this file's/PHASE8_LOG.md's Double Dragon history): safe
  // enough that CreateInstance succeeds and later method calls resolve
  // cleanly, without assuming a real interface shape none of this
  // project's evidence actually supports yet.
  uint32_t unknown_0x01030766_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80044000, /*object=*/0x80045000, /*slot_count=*/40);
  shell_hle.RegisterInstance(0x01030766, unknown_0x01030766_obj);
  // A third real, unidentified class, found continuing the Super
  // BurgerTime investigation past the stack/module collision and the
  // 0x40/0xc static-base slots (TASKS.md Phase 8): real code inside
  // `HandleEvent` (`supbtime.mod` offset 0x11be90-0x11be98) calls
  // `ISHELL_CreateInstance(shell, ClsId=0x01001017, ppObj=&g_2e28fc)`
  // -- a real module-global variable, not a stack slot -- and, like
  // every other real `CreateInstance` call site in this project's
  // history that turned out to matter, never checks the returned
  // status before dereferencing `*ppObj` two instructions later. Since
  // this class wasn't registered, `IShellHle::CreateInstanceImpl`
  // correctly returned failure and correctly left `*ppObj` untouched
  // (matching real `AEEShell.h` semantics) -- but real code reads it
  // anyway, calls a method on the resulting null "object", and crashes.
  // Confirmed via a live memory watchpoint on `g_2e28fc` spanning the
  // entire run (temporary, reverted) that nothing else ever writes
  // there -- this call site is the one and only real source of that
  // value.
  //
  // Its one real call site (`supbtime.mod` offset 0x11be90-0x11be98,
  // immediately after `CreateInstance` succeeds) calls this object's
  // own slot 7 (byte offset 0x1c) with `(this, flag=0x4000,
  // callback=0x11c06c, user_data=0)`. `0x11c06c` is real, disassembled
  // ARM code, not data -- and its own body is a textbook "process a
  // list of registered objects once per call" shape: dereference a
  // real module-global list head; if empty, return immediately; else
  // walk the list calling a vtable method on each entry. That's
  // exactly what a real per-frame "run one engine tick" function looks
  // like, matching this title's own "generic arcade-core" structure
  // (TASKS.md Phase 8) -- and it's registered here but never actually
  // invoked, since a plain no-op stub just returns success without
  // scheduling it, and nothing else in this run ever calls it.
  //
  // EXPERIMENTAL, and a real step beyond every other generic scaffold
  // in this file: rather than leave slot 7 a no-op, this schedules the
  // given callback through the existing, already-real `IShellHle`
  // timer mechanism (the same one Double Dragon/Peggle's own
  // self-rearming `SetTimer` callbacks run through), on the same
  // 16ms cadence `kTickMs` uses elsewhere in this file -- an inferred
  // interval, not one the real call site actually provides. Marked
  // clearly as an inference rather than confirmed real behavior: the
  // *fact* that a callback gets registered here is directly evidenced;
  // the specific interval chosen to drive it is not.
  //
  // A second override is needed for a real, different reason: that
  // list is a single-entry list whose one entry is this very object
  // (`ppObj` from the `CreateInstance` call above, real address
  // `0x002e28fc` -- confirmed live and stable via a temporary memory
  // watchpoint spanning a full run, TASKS.md Phase 8), and `0x11c06c`'s
  // loop has no exit *except* that address reading back 0. Nothing
  // else in this codebase, real or stubbed, ever writes to it after
  // `CreateInstance` -- a real implementation would presumably do so
  // itself, once whatever real work it represents (almost certainly the
  // romset load this class is tied to) finishes.
  //
  // First attempt put this clear on slot 11 (byte offset 0x2c, real
  // disassembly of `0x11c06c` confirms it's the first of *three* real
  // vtable calls `0x11c06c`'s per-entry body makes on this same object
  // every single pass: slot 0x2c ("tick"), then an unrelated object's
  // slot 0x90 (fed slot 0x2c's return value via r1 -- a real, evidenced
  // status hand-off), then slot 0x28 again on this object, all *before*
  // the loop re-reads the list head to decide whether to exit). Clearing
  // the list head from inside slot 0x2c made the very next same-pass
  // call (slot 0x28, still against the now-null `[r4]`) dereference a
  // null vtable unconditionally -- confirmed via a live register trace
  // at the exact call site (`supbtime.mod` 0x11c0cc): call target
  // resolved to `[0x28]` = 0, jumping to address 0, then (this codebase's
  // already-documented "wander through zeroed memory" behavior) walking
  // 262,144 harmless zero-decoded steps until PC coincidentally lands
  // back on the module's own real load address (`kBase`), re-entering
  // and re-running its one-time ROPI relocation-fixup veneer a second
  // time over a table whose backing storage was already zeroed and
  // reclaimed as scratch after its first, legitimate use -- which
  // self-corrupts real code (module offset `0x9c`) into what eventually
  // decodes as an unimplemented `MRS`/`MSR`-space instruction. Not a new,
  // separate CPU gap: a direct, traced consequence of clearing the list
  // head one real sub-call too early.
  //
  // **Fixed** by moving the clear to slot 10 (byte offset 0x28) instead
  // -- the *last* of the three real per-pass sub-calls, called after
  // slot 0x2c and the other object's slot 0x90 have already run against
  // a still-valid, non-null object. Still an honest, minimal placeholder
  // (not a claim about which slot "really" owns cleanup, just the
  // latest-firing of the three already-being-called real slots, chosen
  // specifically so nothing in the same pass dereferences the entry
  // again afterward) -- not a claim that real loading finishes in one
  // frame, just the simplest choice that doesn't require inventing an
  // arbitrary frame count.
  constexpr uint32_t kSbtTaskListHeadAddress = 0x002e28fc;
  std::vector<zeebulator::HleRuntime::HleFunction> sbt_methods(
      40, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
  sbt_methods[7] = [&shell_hle](zeebulator::IArmCore& core) {
    constexpr uint32_t kInferredTickMs = 16;
    uint32_t callback = core.GetRegister(zeebulator::kR2);
    uint32_t user_data = core.GetRegister(zeebulator::kR3);
    shell_hle.ScheduleTimer(kInferredTickMs, callback, user_data);
    core.SetRegister(zeebulator::kR0, 0);  // SUCCESS
  };
  sbt_methods[10] = [](zeebulator::IArmCore& core) {
    core.GetMemory().Write32(kSbtTaskListHeadAddress, 0);
    core.SetRegister(zeebulator::kR0, 0);
  };
  uint32_t unknown_0x01001017_obj = zeebulator::BuildInterfaceObject(
      cpu.GetMemory(), hle, /*vtable_address=*/0x80046000, /*object_address=*/0x80047000,
      sbt_methods);
  shell_hle.RegisterInstance(0x01001017, unknown_0x01001017_obj);
  // Real code fetches "the current app's IShell"/"IDisplay" from an
  // ambient context (the static-base table's offset-0xc0 slot) in many
  // places, not just via the pIShell argument explicitly passed to
  // AEEMod_Load/CreateInstance -- see core/brew/mod_runtime.h.
  mod_runtime.SetShellInstance(shell);
  mod_runtime.SetDisplayInstance(display_obj);
  // The same ambient context struct has a third real field (offset
  // 0x2c) found probing Peggle -- real code there calls through it
  // using ARM RVCT's ROPI relative-vtable convention, unlike every
  // other confirmed interface here. Its real identity is still
  // unknown; wired to a relative-vtable-safe scaffold (see
  // BuildGenericRelativeVtableStubObject's doc comment) purely so the
  // call resolves rather than wandering into unmapped memory.
  uint32_t unknown_context_0x2c_obj = zeebulator::BuildGenericRelativeVtableStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80010000, /*object=*/0x80011000, /*slot_count=*/20);
  mod_runtime.SetThirdContextObject(unknown_context_0x2c_obj);
  // A fifth real field (offset 0x28) found continuing the investigation
  // past the fourth field's arena gate: real code (peggle.mod offset
  // 0x132dfc, called with no null check beforehand) reads it and calls
  // through it using the exact same ROPI relative-vtable convention as
  // the third field above -- see mod_runtime.h's doc comment. Same
  // treatment as the third field: a safe, do-nothing relative-vtable
  // scaffold so the call resolves instead of wandering into unmapped
  // memory.
  uint32_t unknown_context_0x28_obj = zeebulator::BuildGenericRelativeVtableStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80014000, /*object=*/0x80015000, /*slot_count=*/20);
  mod_runtime.SetFifthContextObject(unknown_context_0x28_obj);
  // A fourth real field (offset 0x24) found continuing the Peggle
  // investigation into why its per-tick callback never re-arms its own
  // timer the real self-rearming way: real code there reads and writes
  // this as a plain data struct (not a vtable interface), gating its
  // entire timer-rearming path on offset +20 being non-zero. Real
  // identity unknown -- wired to a real, writable, zeroed memory block
  // with just that one confirmed-load-bearing field pre-set non-zero,
  // an educated, minimal enabling stub (see mod_runtime.h's doc
  // comment), not a confirmed-correct implementation of whatever this
  // struct actually is.
  constexpr uint32_t kFourthContextObject = 0x80020000;
  cpu.GetMemory().Write32(kFourthContextObject + 20, 1);  // rest is already zero
  mod_runtime.SetFourthContextObject(kFourthContextObject);
  // Real code elsewhere (peggle.mod offset 0x989c-0x99f8, reached from
  // the same tick-0 callback once it got past the +20 gate above) reads
  // `context[0x24] + 0x45000 + 0x3d8` as a real, standalone object --
  // confirmed by the call shape that follows it (`ldr r0,[fp]; ldr
  // r3,[r0,#8]; mov r0,fp; bx r3`, i.e. a plain absolute vtable call at
  // slot 2 with `fp` itself as `this`, not the ROPI relative-vtable
  // shape the third context field uses) -- ordinary enough to give a
  // real, generic stub object rather than modeling the rest of this
  // apparent large global arena, which nothing yet requires understood.
  // Written after HandleEvent(EVT_APP_START) below, not here -- real
  // code (confirmed via a live memory watchpoint) writes a real zero
  // to this exact field once during that call, presumably a real
  // "not yet initialized" reset that legitimately precedes whatever
  // real code would normally populate it for real.
  //
  // Real code calls this object's own slot 2 with a shape matching a
  // real QueryInterface-style call (`this`, an id/flag, and a pointer
  // to receive the result) and, without checking the result for
  // failure, immediately dereferences whatever slot 2 wrote there.
  // Live tracing found the exact same shape recur at least twice in a
  // row through freshly-returned objects, with no sign of stopping --
  // so rather than manually re-diagnosing and hand-patching each
  // successive level, every slot of every object below is built the
  // same self-propagating way: succeed (r0=0) and write a fresh object
  // of the same kind into whatever the caller passed as the output
  // pointer (r2), lazily, however deep a real chain of these turns out
  // to go. EXPERIMENTAL and specific to this investigation (TASKS.md
  // Phase 8) -- not yet confirmed as a general real BREW convention,
  // so deliberately kept local to this tool rather than promoted to
  // scaffold_object.h.
  //
  // Initially built every one of the 40 slots this same self-propagating
  // way, which appeared to work for two chained levels before hitting a
  // wall where a real caller read offset 0x30 off what looked like one of
  // these objects directly rather than through its vtable -- but full
  // register-level tracing (temporary, reverted) of that exact call chain
  // (peggle.mod offsets 0x1099e0-0x109aac) showed that was a misdiagnosis:
  // the real bug is that only real slot 2 uses the (this, id, ppOut@r2)
  // shape above. Real slot 3 is a *different*, also-real shape --
  // (this, ppOut@r1), no id argument (confirmed at peggle.mod offset
  // 0x109a98: `ldr r0,[fp]; add r1,sp,#0x24; ldr r2,[r0,#0xc]; mov
  // r0,fp; bx r2`) -- and other real slots (e.g. slot 4, confirmed at
  // offset 0x109a00) are called with no output pointer at all, just
  // leftover garbage sitting in r1/r2 from earlier code. Blindly writing
  // a fresh object into r2 for every slot corrupted whatever r2 happened
  // to hold for those other calls -- including, once, real address 0 --
  // and it was real code later reading back that corrupted memory (not a
  // third real object convention) that produced the offset-0x30 wall.
  // Fixed by only special-casing the two real, evidenced shapes (slot 2
  // via r2, slot 3 via r1, each skipped if the pointer is null -- a real
  // "just checking, don't return anything" pattern also observed at
  // peggle.mod offset 0x109a94's `bl 0x105b50` with r1=r2=0) and leaving
  // every other slot a plain, side-effect-free stub.
  uint32_t next_self_propagating_addr = 0x80030000;
  std::function<uint32_t()> build_self_propagating_stub =
      [&cpu, &hle, &next_self_propagating_addr, &build_self_propagating_stub]() -> uint32_t {
    uint32_t vtable_addr = next_self_propagating_addr;
    uint32_t object_addr = next_self_propagating_addr + 0x800;
    next_self_propagating_addr += 0x1000;
    auto propagate_into = [&cpu, &build_self_propagating_stub](zeebulator::ArmRegister out_reg) {
      return [&cpu, &build_self_propagating_stub, out_reg](zeebulator::IArmCore& core) {
        uint32_t out_ptr = core.GetRegister(out_reg);
        if (out_ptr != 0) {
          cpu.GetMemory().Write32(out_ptr, build_self_propagating_stub());
        }
        core.SetRegister(zeebulator::kR0, 0);
      };
    };
    std::vector<zeebulator::HleRuntime::HleFunction> methods(
        40, [](zeebulator::IArmCore& core) { core.SetRegister(zeebulator::kR0, 0); });
    methods[2] = propagate_into(zeebulator::kR2);
    methods[3] = propagate_into(zeebulator::kR1);
    return zeebulator::BuildInterfaceObject(cpu.GetMemory(), hle, vtable_addr, object_addr,
                                             methods);
  };
  uint32_t unknown_arena_0x453d8_obj = build_self_propagating_stub();
  media_hle.Build(/*vtable=*/0x8000B000);

  auto& mem = cpu.GetMemory();
  // A real stack, well past the loaded module -- ArmInterpreter::Reset()
  // zeroes every register including SP, and the real compiled prologue's
  // first instruction is `STR LR,[SP,#-4]!`; without this, that write
  // corrupts memory near address 0 and every stack-relative access after
  // it, matching exactly the convention tools/mod_probe.cpp already uses.
  // Sized relative to the real module (a fixed `kBase + 0x200000` offset
  // silently collided with real module data for Super BurgerTime's own
  // 2.8MB `.mod` -- see PHASE8_LOG.md for the full real evidence: a real
  // ROPI relocation-fixup table computed from real, file-embedded
  // literals landed squarely inside where that fixed offset put SP,
  // making the table read back as zero mid-walk and self-corrupting
  // real code well before any HLE surface was ever reached).
  cpu.SetRegister(zeebulator::kSP, kBase + mod_size + 0x00200000);

  // AEEMod_Load must be the first thing in the module (real BREW
  // requirement, confirmed against AEEModGen.c in Phase 3) -- and
  // Phase 2's real .mod probing already validated file offset 0 as a
  // coherent function prologue for this exact file.
  uint32_t entry = kBase;

  const char* stage = "AEEMod_Load";
  uint32_t applet_ptr = 0;
  uint32_t handle_event_fn = 0;
  bool injected_simulated_download_complete = false;
  try {
    std::printf("Calling AEEMod_Load...\n");
    constexpr uint32_t kPpModAddr = 0x00090000;
    auto load_result =
        CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, entry, shell, 0, kPpModAddr, 0);
    uint32_t module_ptr = mem.Read32(kPpModAddr);
    if (load_result.wandered_outside_module || load_result.exceeded_step_budget || !module_ptr) {
      std::printf("AEEMod_Load did not produce a trustworthy module pointer -- stopping.\n");
      return 1;
    }
    std::printf("AEEMod_Load OK, module=0x%08x\n", module_ptr);

    stage = "IModule::CreateInstance";
    uint32_t module_vtable = mem.Read32(module_ptr);
    uint32_t create_instance_fn = mem.Read32(module_vtable + 2 * 4);
    std::printf("Calling IModule::CreateInstance(ClsId=%u)...\n", cls_id);
    constexpr uint32_t kPpObjAddr = 0x00090010;
    auto create_result = CallArmFunctionChecked(
        cpu, kTrapBase, kBase, mod_size, create_instance_fn, module_ptr, shell, cls_id, kPpObjAddr);
    // *ppObj is the IApplet* itself, not a function pointer -- HandleEvent
    // is slot 2 of *its* vtable (AddRef=0, Release=1, HandleEvent=2, per
    // the real AEEAppGen.c reference source's IAppletVtbl init order).
    // Same double-indirection already used above for IModule::CreateInstance.
    applet_ptr = mem.Read32(kPpObjAddr);
    uint32_t applet_vtable = mem.Read32(applet_ptr);
    handle_event_fn = mem.Read32(applet_vtable + 2 * 4);
    if (create_result.wandered_outside_module || create_result.exceeded_step_budget ||
        !applet_ptr) {
      std::printf(
          "CreateInstance did not produce a trustworthy applet pointer -- stopping. "
          "(returned %u, *ppObj=0x%08x, wandered=%d, exceeded=%d)\n",
          create_result.r0, applet_ptr, create_result.wandered_outside_module,
          create_result.exceeded_step_budget);
      return 1;
    }
    std::printf("CreateInstance OK, applet=0x%08x HandleEvent=0x%08x\n", applet_ptr,
                handle_event_fn);

    // Real AEEAppStart layout, verified against the real AEEAppStart.h/
    // AEERect.h (NOT the same as our own hello_brew/hello_gl test
    // fixtures' simplified struct -- see PHASE8_LOG.md):
    //   int error; AEECLSID clsApp; IDisplay *pDisplay;
    //   struct { int16 x, y, dx, dy; } rc;  // NOT int -- half the size
    //   const char *pszArgs;                // a field our fixtures lack entirely
    constexpr uint32_t kAppStartAddr = 0x00090020;
    mem.Write32(kAppStartAddr + 0, 0);                                // error
    mem.Write32(kAppStartAddr + 4, cls_id);                           // clsApp
    mem.Write32(kAppStartAddr + 8, display_obj);                      // pDisplay
    mem.Write16(kAppStartAddr + 12, 0);                               // rc.x
    mem.Write16(kAppStartAddr + 14, 0);                               // rc.y
    mem.Write16(kAppStartAddr + 16, static_cast<uint16_t>(kWidth));   // rc.dx
    mem.Write16(kAppStartAddr + 18, static_cast<uint16_t>(kHeight));  // rc.dy
    mem.Write32(kAppStartAddr + 20, 0);                               // pszArgs

    stage = "HandleEvent(EVT_APP_START)";
    constexpr uint32_t kEvtAppStart = 0;  // real value, verified against AEEEvent.h
    std::printf("Calling HandleEvent(EVT_APP_START)...\n");
    // boolean HandleEvent(IApplet *po, AEEEvent evt, uint16 wParam, uint32 dwParam)
    auto handle_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, handle_event_fn,
                                                 applet_ptr, kEvtAppStart, 0, kAppStartAddr);
    if (handle_result.wandered_outside_module || handle_result.exceeded_step_budget) {
      std::printf("HandleEvent(EVT_APP_START) did not complete trustworthily -- stopping.\n");
      return 1;
    }
    std::printf("HandleEvent(EVT_APP_START) returned %u\n", handle_result.r0);
  } catch (const std::exception& e) {
    std::printf("%s threw: %s (pc=0x%08x, offset 0x%08x from mod base)\n", stage, e.what(),
                cpu.GetRegister(zeebulator::kPC), cpu.GetRegister(zeebulator::kPC) - kBase);
    return 1;
  }

  // Real code inside CreateInstance/HandleEvent(EVT_APP_START) writes a
  // real zero to context[0x24]+0x45000+0x3d8 once, presumably its own
  // "not yet initialized" reset -- confirmed via a live memory
  // watchpoint (temporary, reverted). Writing our placeholder object
  // here, after that real reset instead of before it, is what makes it
  // stick for the real per-tick reads that follow.
  cpu.GetMemory().Write32(kFourthContextObject + 0x45000 + 0x3d8, unknown_arena_0x453d8_obj);
  // A sibling arena field, `+0x45000+0x3dc` (immediately after the one
  // above), found tracing why Peggle's steady-state per-tick loop never
  // varies (TASKS.md Phase 8): real code (`peggle.mod` offset
  // 0x132df0-0x132df4, called from the timer callback every single
  // tick) reads it and passes it, unconditionally and un-null-checked,
  // as `this` into a real subroutine (offset 0x109088) that immediately
  // dereferences it (`ldr r0,[r0,#4]`, `str r0,[r5,#4]`, `ldr
  // r0,[r5,#12]`, then `ldr r0,[r0]`). Left at 0 (this codebase's
  // default), that subroutine operates on a real null pointer every
  // tick -- our emulator's memory model tolerates that silently rather
  // than faulting, but it means every one of those accesses reads or
  // writes real, meaningful low addresses (0, 4, 0xc, ...) instead of
  // this field's own memory, which is real, evidenced address-0
  // pollution risk, not simulation of anything real. This struct's real
  // element layout (offset +4 looks like a call counter; +12 looks like
  // a pointer to a small, up-to-4-element array whose entries are read
  // at large offsets like +0xbc/+0xdc) is not understood well enough to
  // populate meaningfully -- likely Peggle's own internal per-tick game
  // data, not a generic BREW interface, and a materially bigger
  // reverse-engineering task than every other field fixed so far. So,
  // rather than guess at that real layout, this gets the same safe,
  // conservative treatment as the fourth field's own arena allocation
  // itself: a real, writable, zeroed memory block, just enough to stop
  // the real null-pointer accesses from landing on unrelated low
  // addresses. This does NOT change what the real subroutine does (a
  // zeroed block still reads as "empty" at every offset checked, so it
  // still takes the same do-nothing branch) -- it only isolates the
  // read/write pattern safely, and is not expected to unblock further
  // real progress on its own.
  constexpr uint32_t kArena0x3dcBlock = 0x80050000;
  cpu.GetMemory().Write32(kFourthContextObject + 0x45000 + 0x3dc, kArena0x3dcBlock);

  std::printf("Reached the event loop with no unhandled instruction! Window will stay open.\n");
  bool running = true;
  SDL_Event event;
  constexpr uint32_t kTickMs = 16;
  uint64_t tick_count = 0;
  while (running) {
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = false;
      if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) && !event.key.repeat) {
        uint32_t avk = SdlKeyToAvk(event.key.keysym.sym);
        if (avk != 0 && applet_ptr != 0) {
          // boolean HandleEvent(IApplet *po, AEEEvent evt, uint16 wParam, uint32 dwParam)
          // evt 0x101/0x102 confirmed via real disassembly of Double
          // Dragon's own event dispatcher -- see SdlKeyToAvk's comment
          // and PHASE8_LOG.md.
          constexpr uint32_t kEvtKeyDown = 0x101;
          constexpr uint32_t kEvtKeyUp = 0x102;
          uint32_t evt = (event.type == SDL_KEYDOWN) ? kEvtKeyDown : kEvtKeyUp;
          try {
            auto key_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size,
                                                       handle_event_fn, applet_ptr, evt, avk, 0);
            std::printf("HandleEvent(evt=0x%x, wParam=0x%x) returned %u%s\n", evt, avk,
                        key_result.r0,
                        key_result.wandered_outside_module ? " (wandered!)" : "");
          } catch (const std::exception& e) {
            std::printf("key event threw: %s (pc=0x%08x, offset 0x%08x from mod base)\n", e.what(),
                        cpu.GetRegister(zeebulator::kPC), cpu.GetRegister(zeebulator::kPC) - kBase);
          }
        }
      }
    }
    // Real BREW timers are one-shot -- real game code re-arms its own via
    // ISHELL_SetTimer from inside the callback (see core/brew/ishell.h).
    // Driving these is what actually runs the game's per-frame logic;
    // nothing calls into the module otherwise from here on.
    mod_runtime.Tick(kTickMs);
    // Simulate a truthful "download/install 100% complete" notification
    // once the real callback has been registered (see the class-0x01005511
    // doc comment above) -- a real event struct shape confirmed via
    // disassembly of the real registered callback (`ddragonz.mod`
    // `0x11d020`): only two fields are read, `+8` (must equal 4) and
    // `+16` (must be 2 or 3; both route to the same real success path).
    // One-shot, not held -- this models a discrete real notification, not
    // continuous input state. Confirmed live (PHASE8_LOG.md) that this
    // reaches the real success-path *check* (`0x11f4dc`) without wandering
    // or throwing, but doesn't yet clear it: that check also requires a
    // real byte at `pUser+37` to be nonzero, and nothing this project has
    // triggered so far ever sets it. Kept as a real, evidence-grounded
    // building block for whoever traces that next -- not yet sufficient
    // on its own.
    if (!injected_simulated_download_complete && *captured_download_callback != 0 &&
        tick_count >= 30) {
      injected_simulated_download_complete = true;
      constexpr uint32_t kSimulatedEventStructAddr = 0x80066000;
      cpu.GetMemory().Write32(kSimulatedEventStructAddr + 8, 4);
      cpu.GetMemory().Write32(kSimulatedEventStructAddr + 16, 2);
      std::printf("  [input] simulating a download-complete notification: invoking callback "
                  "0x%08x\n",
                  *captured_download_callback);
      try {
        CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, *captured_download_callback,
                               *captured_download_context, kSimulatedEventStructAddr, 0, 0);
      } catch (const std::exception& e) {
        std::printf("  [input] download-complete callback threw: %s\n", e.what());
      }
    }
    // Simulate a real, held button press once the game has genuinely
    // registered for button events (captured_button_callback nonzero) and
    // had a fair chance to finish resource loading. Re-fires every tick
    // for a real ~4-second window (ticks 60-300) rather than once: a
    // single momentary press only ever set the gate for one tick before
    // the real per-tick "publish" logic (traced live, `ddragonz.mod`
    // 0x123740) cleared it again, and a real held press is what actually
    // drove the real per-tick state machine (`applet+0x50`/`+0x54`)
    // through multiple distinct, confirmed-live real states -- genuine
    // evidence this matters, not a guess. Queues real, valid button UIDs
    // (confirmed against the real AEEHIDButtons.h Zeebo mapping table)
    // for every real Zeebo action button and the d-pad, plus UID
    // 0x0106C403 -- not one of AEEHIDButtons.h's documented Zeebo
    // buttons, but a real, working case in ddragonz.mod's own compiled
    // dispatch table that remaps to nButtonID=8 (bit 0x100), the exact
    // bit `applet+0x361c`'s real consumer checks for (confirmed several
    // rounds ago by tracing that consumer directly) and the only one of
    // the 16 real cases that produces it (worked out from disassembly,
    // not guessed). Start/HOME deliberately excluded: its real case
    // (`ddragonz.mod` 0x10080c) returns failure without touching the
    // button-info struct at all, aborting the real callback's own
    // event-processing loop immediately and dropping every event queued
    // after it -- confirmed live.
    if (*captured_button_callback != 0 && tick_count >= 60 && tick_count <= 300) {
      *simulated_button_events = {
          {0, 1, 0x0106C40A},  // Button_1 (ZEEBO_BUTTON_WEST)
          {1, 1, 0x0106C40B},  // Button_2 (ZEEBO_BUTTON_SOUTH)
          {2, 1, 0x0106C40C},  // Button_3 (ZEEBO_BUTTON_NORTH)
          {3, 1, 0x0106C40D},  // Button_4 (ZEEBO_BUTTON_EAST)
          {4, 1, 0x0106C3FE},  // DPAD_UP
          {5, 1, 0x0106C3FF},  // DPAD_LEFT
          {6, 1, 0x0106C400},  // DPAD_DOWN
          {7, 1, 0x0106C401},  // DPAD_RIGHT
          {8, 1, 0x0106C403},  // unnamed but real; the one case that sets bit 0x100
      };
      if (tick_count == 60) {
        std::printf("  [input] holding a simulated button press (ticks 60-300): invoking "
                    "button callback 0x%08x\n",
                    *captured_button_callback);
      }
      try {
        CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, *captured_button_callback,
                               *captured_button_context, 0, 0, 0);
      } catch (const std::exception& e) {
        std::printf("  [input] button callback threw: %s\n", e.what());
      }
    }
    for (const auto& timer : shell_hle.Tick(kTickMs)) {
      bool trace_this_tick = tick_count < 10;
      if (trace_this_tick) std::printf("--- tick %llu ---\n", static_cast<unsigned long long>(tick_count));
      try {
        auto tick_result = CallArmFunctionChecked(cpu, kTrapBase, kBase, mod_size, timer.callback,
                                                   timer.user_data, 0, 0, 0,
                                                   /*trace=*/false,
                                                   /*hle_trace=*/trace_this_tick);
        if (tick_result.wandered_outside_module || tick_result.exceeded_step_budget) {
          std::printf("timer callback did not complete trustworthily -- stopping.\n");
          running = false;
          break;
        }
      } catch (const std::exception& e) {
        std::printf("timer callback threw: %s (pc=0x%08x, offset 0x%08x from mod base)\n",
                    e.what(), cpu.GetRegister(zeebulator::kPC),
                    cpu.GetRegister(zeebulator::kPC) - kBase);
        running = false;
        break;
      }
      ++tick_count;
    }
    mixer.Mix(backend, static_cast<size_t>(kAudioSampleRate * kTickMs / 1000));
    SDL_Delay(kTickMs);
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
