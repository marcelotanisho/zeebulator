// Standalone dev/debug frontend. Currently boots the bundled hello_brew
// M0 demo app (see tests/fixtures/hello_brew/) through the real BREW
// app-lifecycle contract and shows the result in an actual SDL2 window
// -- this is PRD.md's M0 milestone. Loading arbitrary real games is
// later work (needs the .mod entry-point-discovery and GGZ/MIF wiring
// from later phases); for now this frontend exists to prove the whole
// pipeline (CPU core -> HLE -> Backend -> window) actually works.

#include <SDL.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "core/brew/gl_hle.h"
#include "core/brew/hle_runtime.h"
#include "core/brew/idisplay.h"
#include "core/brew/ishell.h"
#include "core/cpu/arm_interpreter.h"
#include "core/loader/mod.h"
#include "frontends/standalone/sdl2_gl_backend.h"
#include "tests/fixtures/hello_brew/entry_offset.h"

namespace {

class Sdl2Backend : public zeebulator::Backend {
 public:
  Sdl2Backend(SDL_Renderer* renderer, int width, int height)
      : renderer_(renderer),
        texture_(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                    SDL_TEXTUREACCESS_STREAMING, width, height)) {}
  ~Sdl2Backend() override { SDL_DestroyTexture(texture_); }

  void PushVideoFrame(const void* framebuffer, int width, int height,
                       zeebulator::PixelFormat format) override {
    (void)format;  // IDisplayHle's framebuffer is always RGB565 for now.
    SDL_UpdateTexture(texture_, nullptr, framebuffer, width * 2);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
  }
  void PushAudioSamples(const int16_t*, size_t) override {}
  zeebulator::ZPadState PollInput() override { return {}; }

 private:
  SDL_Renderer* renderer_;
  SDL_Texture* texture_;
};

}  // namespace

int main(int argc, char** argv) {
  const char* fixture_path = argc > 1 ? argv[1] : HELLO_BREW_FIXTURE_PATH;

  std::ifstream in(fixture_path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: couldn't open '%s'\n", fixture_path);
    return 1;
  }
  std::vector<uint8_t> mod_data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  constexpr int kWidth = 640;
  constexpr int kHeight = 480;  // Zeebo's native output resolution.
  SDL_Window* window =
      SDL_CreateWindow("Zeebulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                        kWidth, kHeight, SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

  zeebulator::ArmInterpreter cpu;
  zeebulator::HleRuntime hle(cpu, 0xF0000000, 0x10000);
  Sdl2Backend backend(renderer, kWidth, kHeight);
  zeebulator::IDisplayHle display(backend, kWidth, kHeight);

  constexpr uint32_t kBase = 0x00100000;
  zeebulator::LoadMod(cpu, mod_data, kBase);

  uint32_t shell = zeebulator::BuildIShell(cpu.GetMemory(), hle, /*vtable=*/0x80000000,
                                            /*object=*/0x80001000);
  uint32_t display_obj =
      display.Build(cpu.GetMemory(), hle, /*vtable=*/0x80002000, /*object=*/0x80003000);

  uint32_t entry = kBase + kHelloBrewAeeModLoadOffset;

  // Drive the real app lifecycle: AEEMod_Load -> IModule::CreateInstance
  // -> HandleEvent(EVT_APP_START). See TASKS.md Phase 3 for the contract.
  constexpr uint32_t kPpModAddr = 0x00090000;
  hle.CallArmFunction(entry, /*pIShell=*/shell, /*ph=*/0, /*ppMod=*/kPpModAddr);
  uint32_t module_ptr = cpu.GetMemory().Read32(kPpModAddr);

  uint32_t module_vtable = cpu.GetMemory().Read32(module_ptr);
  uint32_t create_instance_fn = cpu.GetMemory().Read32(module_vtable + 2 * 4);

  constexpr uint32_t kPpObjAddr = 0x00090010;
  constexpr uint32_t kClsId = 0x1234;
  hle.CallArmFunction(create_instance_fn, module_ptr, shell, kClsId, kPpObjAddr);
  uint32_t handle_event_fn = cpu.GetMemory().Read32(kPpObjAddr);

  constexpr uint32_t kAppStartAddr = 0x00090020;
  auto& mem = cpu.GetMemory();
  mem.Write32(kAppStartAddr + 0, 0);            // error
  mem.Write32(kAppStartAddr + 4, kClsId);       // clsApp
  mem.Write32(kAppStartAddr + 8, display_obj);  // pDisplay
  mem.Write32(kAppStartAddr + 12, 0);
  mem.Write32(kAppStartAddr + 16, 0);
  mem.Write32(kAppStartAddr + 20, 0);
  mem.Write32(kAppStartAddr + 24, 0);

  constexpr uint32_t kTestEvtAppStart = 1;  // matches hello_brew.c's own constant
  hle.CallArmFunction(handle_event_fn, /*pMe=*/0, kTestEvtAppStart, /*wParam=*/0,
                       kAppStartAddr);

  std::printf("Zeebulator: hello_brew booted -- window should show a white block\n");

  // --- Phase 5 GL smoke test -----------------------------------------
  // Separate window, separate real GL context: proves the IGL/IEGL HLE
  // vtables actually reach a real host OpenGL context end to end (real
  // vtable dispatch -> GlHle -> Sdl2GlBackend -> real desktop GL calls ->
  // a visible pixel), independent of the hello_brew/IDisplay path above.
  // No compiled ARM binary drives this yet (that's TASKS.md Phase 5's
  // next item, "validate against SDK sample apps") -- these calls are
  // made the same way the app-lifecycle calls above are, directly via
  // HleRuntime::CallArmFunction, exercising the exact same vtable/HLE
  // dispatch path real compiled code would use.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  SDL_Window* gl_window =
      SDL_CreateWindow("Zeebulator - GL smoke test", SDL_WINDOWPOS_CENTERED,
                        SDL_WINDOWPOS_CENTERED, kWidth, kHeight, SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL);

  zeebulator::Sdl2GlBackend gl_backend(gl_window);
  zeebulator::GlHle gl_hle(gl_backend);
  uint32_t gl_obj = gl_hle.BuildGl(cpu.GetMemory(), hle, /*vtable=*/0x80004000,
                                    /*object=*/0x80005000);
  uint32_t egl_obj = gl_hle.BuildEgl(cpu.GetMemory(), hle, /*vtable=*/0x80006000,
                                      /*object=*/0x80007000);
  (void)gl_obj;

  auto EglSlot = [&](uint32_t slot) { return mem.Read32(0x80006000 + slot * 4); };
  auto GlSlot = [&](uint32_t slot) { return mem.Read32(0x80004000 + slot * 4); };

  uint32_t egl_display = hle.CallArmFunction(EglSlot(4), 0);              // eglGetDisplay
  hle.CallArmFunction(EglSlot(5), egl_display, 0, 0);                     // eglInitialize
  uint32_t configs_out = 0x00091000, num_config_out = 0x00091004;
  uint32_t stack_args = 0x00091008;
  cpu.SetRegister(zeebulator::kSP, stack_args);
  mem.Write32(stack_args, num_config_out);  // eglChooseConfig's 5th arg (num_config*)
  hle.CallArmFunction(EglSlot(10), egl_display, 0, configs_out, 1);       // eglChooseConfig
  uint32_t config = mem.Read32(configs_out);
  uint32_t surface =
      hle.CallArmFunction(EglSlot(12), egl_display, config, 0, 0);       // eglCreateWindowSurface
  uint32_t context =
      hle.CallArmFunction(EglSlot(17), egl_display, config, 0, 0);       // eglCreateContext
  hle.CallArmFunction(EglSlot(19), egl_display, surface, surface, context);  // eglMakeCurrent

  // Distinct teal clear color -- unambiguous against both black (no-op)
  // and white (the hello_brew window's block) in a screenshot.
  constexpr uint32_t kFixedOne = 1u << 16;
  hle.CallArmFunction(GlSlot(8), 0, kFixedOne / 2, kFixedOne / 2, kFixedOne);  // glClearColorx
  hle.CallArmFunction(GlSlot(7), 0x4000);                                     // glClear(GL_COLOR_BUFFER_BIT)
  hle.CallArmFunction(EglSlot(26), egl_display, surface);                     // eglSwapBuffers

  std::printf(
      "Zeebulator: GL smoke test done -- second window should show a solid teal fill\n");
  (void)egl_obj;

  bool running = true;
  SDL_Event event;
  while (running) {
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = false;
    }
    SDL_Delay(16);
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_DestroyWindow(gl_window);
  SDL_Quit();
  return 0;
}
