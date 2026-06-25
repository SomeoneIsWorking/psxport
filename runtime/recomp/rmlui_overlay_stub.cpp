// rmlui_overlay_stub.cpp — the RmlUi mod/debug overlay is DROPPED from the SDL_GPU build (its SDL2 +
// Vulkan-render backend doesn't port to SDL3/SDL_GPU in this pass; re-add later if wanted). These stubs
// satisfy the few overlay symbols other TUs reference (menu.cpp, pad_input.cpp):
//   - rmlui_overlay_inited() == 0  → menu.cpp falls back to the faithful in-game Options menu.
//   - rmlui_overlay_wants_keyboard() == 0 → pad_input.cpp never suppresses gameplay keys.
// set_visible/set_options_mode are no-ops (no overlay to show). See docs/render-backend-port.md.
extern "C" {
int  rmlui_overlay_inited(void) { return 0; }
int  rmlui_overlay_wants_keyboard(void) { return 0; }
void rmlui_overlay_set_visible(int v) { (void)v; }
void rmlui_overlay_set_options_mode(int v) { (void)v; }
void rmlui_overlay_set_world(int x, int y, int z, unsigned stage) { (void)x; (void)y; (void)z; (void)stage; }
}
