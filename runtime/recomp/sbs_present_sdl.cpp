// sbs_present_sdl.cpp — the SBS two-pane presenter on SDL_GPU (replaces the dropped raylib presenter).
//
// The SDL_GPU renderer (gpu_gpu.cpp) owns the single window. The SBS harness (sbs.cpp) renders each core's
// frame headless-style into the VRAM image and reads it back to a CPU RGBA pane (gpu_gpu_render_readback),
// then this presents the two panes side by side in one window frame (gpu_gpu_present_sbs2). Window I/O —
// the quit poll and host-keyboard -> PSX pad mask — is driven here off SDL3 directly. No second window
// (the old raylib path opened its own, hence the SBS forced VK headless; that is no longer needed).
#include <SDL3/SDL.h>
#include <stdint.h>

// gpu_gpu.cpp: composite two CPU RGBA8 panes (A left, B right) to the swapchain in one window frame.
void gpu_gpu_present_sbs2(const uint8_t* rgbaA, int wA, int hA, const uint8_t* rgbaB, int wB, int hB);

extern "C" {
void sbs_rl_init(void) {}        // the SDL_GPU renderer creates the window lazily on the first present
void sbs_rl_shutdown(void) {}

// Return 1 if the window was closed (SDL quit). gpu_gpu_present_sbs2 also exits(0) on quit, so this is a
// belt-and-suspenders check for frames where no present ran (e.g. paused).
int sbs_rl_should_close(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) return 1;
  return 0;
}

// Host keyboard -> active-low PSX button mask (same mapping as runtime/recomp/pad_input.cpp). Mirrored to
// both cores by sbs.cpp::feed_input.
unsigned short sbs_rl_poll_input(void) {
  SDL_PumpEvents();
  const bool* ks = SDL_GetKeyboardState(NULL);
  uint16_t m = 0xFFFF;
  if (!ks) return m;
  #define KD(sc) (ks[(sc)])
  if (KD(SDL_SCANCODE_UP)    || KD(SDL_SCANCODE_W)) m &= ~0x0010u;   // Up
  if (KD(SDL_SCANCODE_RIGHT) || KD(SDL_SCANCODE_D)) m &= ~0x0020u;   // Right
  if (KD(SDL_SCANCODE_DOWN)  || KD(SDL_SCANCODE_S)) m &= ~0x0040u;   // Down
  if (KD(SDL_SCANCODE_LEFT)  || KD(SDL_SCANCODE_A)) m &= ~0x0080u;   // Left
  if (KD(SDL_SCANCODE_RETURN))                      m &= ~0x0008u;   // Start
  if (KD(SDL_SCANCODE_RSHIFT)|| KD(SDL_SCANCODE_TAB)) m &= ~0x0001u; // Select
  if (KD(SDL_SCANCODE_K)) m &= ~0x4000u;   // Cross
  if (KD(SDL_SCANCODE_L)) m &= ~0x2000u;   // Circle
  if (KD(SDL_SCANCODE_I)) m &= ~0x1000u;   // Triangle
  if (KD(SDL_SCANCODE_J)) m &= ~0x8000u;   // Square
  if (KD(SDL_SCANCODE_Q)) m &= ~0x0400u;   // L1
  if (KD(SDL_SCANCODE_E)) m &= ~0x0800u;   // R1
  #undef KD
  return m;
}

void sbs_rl_present(const unsigned char* a, int wA, int hA, const unsigned char* b, int wB, int hB) {
  gpu_gpu_present_sbs2(a, wA, hA, b, wB, hB);
}
}
