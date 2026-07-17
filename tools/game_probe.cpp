// Dev tool: drives a real BREW game's full lifecycle (AEEMod_Load ->
// IModule::CreateInstance -> HandleEvent(EVT_APP_START)) through every
// HLE interface implemented so far (IShell, IDisplay, IFile/IFileMgr,
// IGL/IEGL, IMedia), to see how far real execution actually gets and
// exactly which gap it hits next -- the "iteratively debug against the
// real game" approach TASKS.md Phase 8 describes. Real-file validation
// only: takes paths at runtime, never embeds/bundles game content (see
// CONTRIBUTING.md's clean-room policy).

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "core/audio/mixer.h"
#include "core/brew/file_hle.h"
#include "core/brew/gl_hle.h"
#include "core/brew/idisplay.h"
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

void MergeGgzInto(zeebulator::VirtualFilesystem& vfs, const char* path) {
  auto archive = zeebulator::GgzArchive::Parse(ReadFile(path));
  for (const auto& entry : archive.Entries()) {
    vfs.AddFile(entry.name, archive.Extract(entry));
  }
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
// Dragon's real AEEMod_Load (see TASKS.md Phase 8): a missing loader
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
          "TASKS.md Phase 8)\n",
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
// real disassembly (TASKS.md Phase 8) is that Double Dragon's own
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
  // TASKS.md Phase 8 for how this was found via real disassembly.
  zeebulator::ModRuntime mod_runtime(cpu.GetMemory(), hle, /*heap_region=*/0x80300000,
                                      /*heap_size=*/0x00100000, /*context_address=*/0x80280200);
  mod_runtime.Install(kBase, /*table_address=*/0x80280000);

  uint32_t display_obj =
      display.Build(cpu.GetMemory(), hle, /*vtable=*/0x80002000, /*object=*/0x80003000);
  // Real compiled app code obtains IDisplay through
  // ISHELL_CreateInstance(AEECLSID_DISPLAY, ...), not directly -- found
  // via real disassembly of AEEApplet_New's call chain (TASKS.md Phase 8).
  zeebulator::IShellHle shell_hle(cpu.GetMemory(), hle);
  shell_hle.RegisterInstance(/*AEECLSID_DISPLAY=*/0x01001001, display_obj);
  // ClsId 0x01002001: a real BREW class Double Dragon's own graphics-init
  // routine requires (ISHELL_CreateInstance failing for it is the
  // confirmed root cause of the "memory insufficient" dead end -- see
  // TASKS.md Phase 8). Its real interface isn't identified yet, so this
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
  // either.
  uint32_t device_bitmap_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x8000E000, /*object=*/0x8000F000, /*slot_count=*/20);
  display.SetDeviceBitmapInstance(device_bitmap_obj);
  // Two more unidentified real BREW classes a later init routine requires
  // (found by the same technique: real disassembly of 0x1b2fc showed two
  // more ISHELL_CreateInstance calls gating the same "memory
  // insufficient" state -- see TASKS.md Phase 8). Neither is dereferenced
  // within that routine itself, so a generic scaffold is enough to clear
  // this specific gate; extend if/when real code calls a method on one.
  uint32_t unknown_0x01001003_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80010000, /*object=*/0x80011000, /*slot_count=*/40);
  shell_hle.RegisterInstance(0x01001003, unknown_0x01001003_obj);
  uint32_t unknown_0x01001014_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80012000, /*object=*/0x80013000, /*slot_count=*/40);
  shell_hle.RegisterInstance(0x01001014, unknown_0x01001014_obj);
  // A still-deeper gate (0x1d5b8, reached only after the fixes above)
  // requires two more classes -- confirmed via real objdump directly on
  // the literal pool addresses its own `ldr r1,[pc,#N]` instructions
  // reference (0x1d970/0x1d974), not assumed from the nearby-looking
  // 0x0100100x classes above (a first attempt reused those by mistake
  // and was caught because the resulting crash's real disassembly showed
  // different literal values at those addresses).
  uint32_t unknown_0x01014bc3_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80014000, /*object=*/0x80015000, /*slot_count=*/40);
  shell_hle.RegisterInstance(0x01014bc3, unknown_0x01014bc3_obj);
  uint32_t unknown_0x01014bc4_obj = zeebulator::BuildGenericStubObject(
      cpu.GetMemory(), hle, /*vtable=*/0x80016000, /*object=*/0x80017000, /*slot_count=*/40);
  shell_hle.RegisterInstance(0x01014bc4, unknown_0x01014bc4_obj);
  // Real code fetches "the current app's IShell"/"IDisplay" from an
  // ambient context (the static-base table's offset-0xc0 slot) in many
  // places, not just via the pIShell argument explicitly passed to
  // AEEMod_Load/CreateInstance -- see core/brew/mod_runtime.h.
  mod_runtime.SetShellInstance(shell);
  mod_runtime.SetDisplayInstance(display_obj);
  file_hle.Build(/*file_mgr_vtable=*/0x80004000, /*file_mgr_object=*/0x80005000,
                  /*file_vtable=*/0x80006000);
  gl_hle.BuildGl(cpu.GetMemory(), hle, /*vtable=*/0x80007000, /*object=*/0x80008000);
  gl_hle.BuildEgl(cpu.GetMemory(), hle, /*vtable=*/0x80009000, /*object=*/0x8000A000);
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
    // fixtures' simplified struct -- see TASKS.md Phase 8):
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
          // and TASKS.md Phase 8.
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
                                                   timer.user_data, 0, 0, 0, /*trace=*/false,
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
