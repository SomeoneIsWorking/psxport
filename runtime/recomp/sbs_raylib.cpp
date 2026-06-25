// sbs_raylib.cpp — raylib (OpenGL) window + presenter for the PSXPORT_SBS two-pane debugger.
//
// WHY: the SBS debugger composited its two panes through a dedicated two-viewport Vulkan swapchain pass
// (gpu_vk.cpp present_sbs). On Apple/MoltenVK that composite rendered BLACK at every stage (while plain
// single-core ./run.sh, which uses the simple single-pane present + headless readback, is fine on the same
// Mac). Rather than keep fighting the MoltenVK present path, the SBS now presents through raylib/OpenGL
// (native + solid on macOS). Each core is still rendered NATIVE-vs-PSX by the PROVEN per-core VK headless
// render (gpu_vk_render_readback → s_tex → CPU RGBA), then raylib just blits the two CPU buffers side by
// side. The single-core game keeps its SDL+VK window untouched.
//
// This TU is deliberately ISOLATED: it includes ONLY raylib + the C stdlib and exposes a small extern "C"
// API. raylib's public types (Rectangle/Color/Texture2D/…) collide with X11/Vulkan/engine symbols, so it
// must not pull in game.h / SDL / Vulkan headers. sbs.cpp talks to it through the prototypes below.

#include "raylib.h"
#include <cstdint>
#include <cstdio>

namespace {

bool      s_open = false;
Texture2D s_tex_a = {0}, s_tex_b = {0};
int       s_aw = 0, s_ah = 0, s_bw = 0, s_bh = 0;   // current texture sizes (recreate on change)

// (Re)create a pane texture at w×h RGBA8 when the incoming frame size changes.
void ensure_tex(Texture2D* t, int* cw, int* ch, int w, int h) {
  if (*cw == w && *ch == h && t->id) return;
  if (t->id) UnloadTexture(*t);
  Image img = GenImageColor(w, h, BLACK);
  ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  *t = LoadTextureFromImage(img);
  UnloadImage(img);
  *cw = w; *ch = h;
}

// Fit a w×h image into the box [bx,bw]×[by,bh] preserving aspect (letterbox/pillarbox), centered.
Rectangle fit(int bx, int by, int bw, int bh, int w, int h) {
  float s = (float)bw / w; if ((float)h * s > bh) s = (float)bh / h;
  float dw = w * s, dh = h * s;
  return (Rectangle){ bx + (bw - dw) * 0.5f, by + (bh - dh) * 0.5f, dw, dh };
}

} // namespace

extern "C" {

void sbs_rl_init(void) {
  if (s_open) return;
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
  SetTraceLogLevel(LOG_WARNING);                 // quiet raylib's per-init INFO spew
  InitWindow(1280, 480, "Tomba!2 SBS  —  A: native  |  B: PSX");
  s_open = true;
}

int sbs_rl_should_close(void) { return s_open ? WindowShouldClose() : 1; }

// Build the active-low PSX pad mask from the keyboard (mirrors pad_input.cpp pad_poll_sdl's mapping).
// Active-low: a bit is CLEARED when its control is pressed. Both panes are fed this same mask.
unsigned short sbs_rl_poll_input(void) {
  unsigned short m = 0xFFFF;
  if (IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W)) m &= ~0x0010;  // Up
  if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) m &= ~0x0020;  // Right
  if (IsKeyDown(KEY_DOWN)  || IsKeyDown(KEY_S)) m &= ~0x0040;  // Down
  if (IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A)) m &= ~0x0080;  // Left
  if (IsKeyDown(KEY_ENTER))                     m &= ~0x0008;  // Start
  if (IsKeyDown(KEY_RIGHT_SHIFT) || IsKeyDown(KEY_TAB)) m &= ~0x0001;  // Select
  if (IsKeyDown(KEY_K)) m &= ~0x4000;  // Cross
  if (IsKeyDown(KEY_L)) m &= ~0x2000;  // Circle
  if (IsKeyDown(KEY_I)) m &= ~0x1000;  // Triangle
  if (IsKeyDown(KEY_J)) m &= ~0x8000;  // Square
  if (IsKeyDown(KEY_Q)) m &= ~0x0400;  // L1
  if (IsKeyDown(KEY_E)) m &= ~0x0800;  // R1
  if (IsKeyDown(KEY_ONE))   m &= ~0x0100;  // L2
  if (IsKeyDown(KEY_THREE)) m &= ~0x0200;  // R2
  return m;
}

// Upload the two CPU RGBA8 frames and draw them side by side (A left, B right), each letterboxed to its
// own aspect within its half of the window, with a thin divider. One window frame.
void sbs_rl_present(const unsigned char* rgbaA, int wA, int hA,
                    const unsigned char* rgbaB, int wB, int hB) {
  if (!s_open) return;
  if (rgbaA && wA > 0 && hA > 0) { ensure_tex(&s_tex_a, &s_aw, &s_ah, wA, hA); UpdateTexture(s_tex_a, rgbaA); }
  if (rgbaB && wB > 0 && hB > 0) { ensure_tex(&s_tex_b, &s_bw, &s_bh, wB, hB); UpdateTexture(s_tex_b, rgbaB); }

  int W = GetScreenWidth(), H = GetScreenHeight();
  int half = W / 2;
  BeginDrawing();
  ClearBackground(BLACK);
  if (s_tex_a.id && s_aw > 0)
    DrawTexturePro(s_tex_a, (Rectangle){0,0,(float)s_aw,(float)s_ah}, fit(0, 0, half, H, s_aw, s_ah),
                   (Vector2){0,0}, 0.0f, WHITE);
  if (s_tex_b.id && s_bw > 0)
    DrawTexturePro(s_tex_b, (Rectangle){0,0,(float)s_bw,(float)s_bh}, fit(half, 0, W - half, H, s_bw, s_bh),
                   (Vector2){0,0}, 0.0f, WHITE);
  DrawRectangle(half - 1, 0, 2, H, DARKGRAY);    // divider
  EndDrawing();                                  // swaps + polls input for the next frame
}

void sbs_rl_shutdown(void) {
  if (!s_open) return;
  if (s_tex_a.id) UnloadTexture(s_tex_a);
  if (s_tex_b.id) UnloadTexture(s_tex_b);
  CloseWindow();
  s_open = false;
}

} // extern "C"
