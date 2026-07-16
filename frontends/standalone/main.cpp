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

#include "core/brew/hle_runtime.h"
#include "core/brew/idisplay.h"
#include "core/brew/ishell.h"
#include "core/cpu/arm_interpreter.h"
#include "core/loader/mod.h"
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
  SDL_Quit();
  return 0;
}
