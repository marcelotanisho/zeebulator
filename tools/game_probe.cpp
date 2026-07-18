// Dev tool: drives a real BREW game's full lifecycle (AEEMod_Load ->
// IModule::CreateInstance -> HandleEvent(EVT_APP_START)) through every
// HLE interface implemented so far (IShell, IDisplay, IFile/IFileMgr,
// IGL/IEGL, IMedia), to see how far real execution actually gets and
// exactly which gap it hits next -- the "iteratively debug against the
// real game" approach documented in PHASE8_LOG.md. Real-file validation
// only: takes paths at runtime, never embeds/bundles game content (see
// CONTRIBUTING.md's clean-room policy).

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
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
          "PHASE8_LOG.md)\n",
          pc, mod_base, mod_base + mod_size, static_cast<unsigned long long>(steps));
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
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
  // reporting zero connected devices (not a guess -- it's the true
  // answer here) is enough to satisfy this gate.
  uint32_t hid_obj = zeebulator::BuildStubObjectWithOverride(
      cpu.GetMemory(), hle, /*vtable=*/0x8001C000, /*object=*/0x8001D000, /*slot_count=*/10,
      /*override_slot=*/7,
      [](zeebulator::IArmCore& core) {
        // AEEResult GetConnectedDevices(IHID*, int nDeviceType,
        //   int *pnDevHandles, int pnDevHandlesLen, int *pnDevHandlesLenReq)
        uint32_t num_handles_req_addr = zeebulator::HleRuntime::ReadStackArg(core, 0);
        if (num_handles_req_addr != 0) {
          core.GetMemory().Write32(num_handles_req_addr, 0);
        }
        core.SetRegister(zeebulator::kR0, 0);  // AEE_SUCCESS
      });
  shell_hle.RegisterInstance(/*AEECLSID_HID=*/0x0106c411, hid_obj);
  // A second class this same routine unconditionally requires next
  // (gated on GetConnectedDevices' own success, not on device count --
  // still reached with zero devices). Very likely AEECLSID_SignalCBFactory:
  // the bundled ZeeboDeveloperGuide0.97.pdf's own IHID walkthrough
  // creates exactly this class immediately after AEECLSID_HID, to build
  // the ISignal objects IHID's connect/button notifications need --
  // matching call order, but (like AEECLSID_DIB above) not a confirmed
  // literal match, so still a generic scaffold rather than assumed
  // real behavior.
  uint32_t unknown_0x01041207_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8001E000, /*object=*/0x8001F000, /*slot_count=*/20);
  shell_hle.RegisterInstance(0x01041207, unknown_0x01041207_obj);
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
  // Confirmed working for two real chained levels (verified via live
  // trace), then hit a real, different-in-kind wall this mechanism
  // can't paper over: a real caller reads offset 0x30 *directly* off
  // one of these returned objects (`ldr r3,[r1,#0x30]`), not through
  // `*object` the way every vtable call up to this point has -- i.e.
  // it expects a flat struct with a real function pointer embedded at
  // a specific fixed offset, not another vtable object. Left as-is
  // (harmlessly reads 0 there and reports the same clean, diagnosable
  // wander as before) rather than guessed at -- the next concrete step
  // for whoever picks this back up.
  uint32_t next_self_propagating_addr = 0x80030000;
  std::function<uint32_t()> build_self_propagating_stub =
      [&cpu, &hle, &next_self_propagating_addr, &build_self_propagating_stub]() -> uint32_t {
    uint32_t vtable_addr = next_self_propagating_addr;
    uint32_t object_addr = next_self_propagating_addr + 0x800;
    next_self_propagating_addr += 0x1000;
    std::vector<zeebulator::HleRuntime::HleFunction> methods(40);
    for (auto& method : methods) {
      method = [&cpu, &build_self_propagating_stub](zeebulator::IArmCore& core) {
        uint32_t out_ptr = core.GetRegister(zeebulator::kR2);
        uint32_t child = build_self_propagating_stub();
        cpu.GetMemory().Write32(out_ptr, child);
        core.SetRegister(zeebulator::kR0, 0);
      };
    }
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
  cpu.SetRegister(zeebulator::kSP, kBase + 0x00200000);

  // AEEMod_Load must be the first thing in the module (real BREW
  // requirement, confirmed against AEEModGen.c in Phase 3) -- and
  // Phase 2's real .mod probing already validated file offset 0 as a
  // coherent function prologue for this exact file.
  uint32_t entry = kBase;

  const char* stage = "AEEMod_Load";
  uint32_t applet_ptr = 0;
  uint32_t handle_event_fn = 0;
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
