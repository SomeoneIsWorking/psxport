// class RmlOverlay — RmlUi's LIFETIME on the port's SDL_GPU device, and nothing else.
//
// One per Game (`c->game->rml_overlay.method()`). It owns the render interface, the SDL system
// interface, the font faces, the context and the loaded document — and it hands the document
// straight to `psx::ui::MenuDocument`, which owns every element, listener and piece of UI state.
// The back-pointer to Game is wired in Game().
//
// THE SPLIT, and why it is the whole point: this class used to be 592 lines in which RmlUi setup,
// tab selection, row value formatting, the `Mods` toggle/adjust ladders, the dev area warp, the
// four readouts, focus navigation and the SDL key switch were one object. Its UI could not be
// constructed, inspected or tested without an SDL_GPU device, so nothing about the menu was
// reachable from a hermetic test, and "the overlay is broken" had 592 lines and no seams.
// Everything DOM-shaped now lives under `runtime/ui/` as components on `psx::ui::Component`
// (modelled on Dusklight's `src/dusk/ui/`, CC0 — see runtime/ui/ui_component.h for what was taken
// and the one place ours deliberately differs).
//
// In SBS with two Games only one Game's overlay wins the SDL window; the other's `mInited` stays
// false and all methods no-op. In standalone the sole Game owns the UI.
#ifndef PSXPORT_RMLUI_OVERLAY_H
#define PSXPORT_RMLUI_OVERLAY_H
#include <SDL3/SDL.h>
#include <cstdint>
#include <string>

#ifdef __cplusplus
#include <memory>

class Game;

namespace psx::ui {
class MenuDocument;
}

class RmlOverlay {
public:
  Game *game = nullptr; // back-pointer wired by Game()

  RmlOverlay();
  ~RmlOverlay();
  RmlOverlay(const RmlOverlay &) = delete;
  RmlOverlay &operator=(const RmlOverlay &) = delete;

  // Bring RmlUi up on the port's existing SDL_GPU device. Call once after the device exists.
  // No-op if already inited.
  //
  // THE WINDOW IS OPTIONAL AND `win` MAY BE NULL. Whether the UI EXISTS must not depend on whether
  // there is a window (docs/workspace/PROTOCOL.md: "the window is an output sink, not a mode") — this used
  // to be called under `if (!s_headless)`, which made every overlay failure invisible to every
  // headless instrument, and that is precisely why a user-reported dead overlay could not be
  // diagnosed without taking the user's screen. `win` is used ONLY for input translation and the
  // SDL system interface; it is never the source of a size.
  //
  // `sink_w`/`sink_h` are the SINK's size, measured by the caller and passed in explicitly — never
  // re-derived here from SDL_GetWindowSize, which exists in one leg only. `target_fmt` is the
  // colour format of the pass the overlay will record into (swapchain windowed, present image
  // headless).
  void init(SDL_Window *win, SDL_GPUDevice *dev, SDL_GPUTextureFormat target_fmt, int sink_w, int sink_h);
  void shutdown();

  // Feed every SDL event (ESC toggles the menu; F1 debugger). Safe if not inited.
  void event(const SDL_Event *e);
  // CPU-side per-frame update (RmlUi context + live row/readout refresh). Safe if not inited.
  void newFrame();
  // Record the menu geometry into the present render pass. No-op when menu is hidden / not inited.
  void recordGpu(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int win_w, int win_h);

  // `inited()` = RmlUi is up and owns resources. It is NOT "there is a menu": LoadDocument can fail
  // (typically a missing PSXPORT_ASSET_DIR) and leave a live context with no document. Ask
  // `hasMenu()` before anything that assumes a UI exists — conflating the two is what let a failed
  // asset load still log "overlay up".
  bool inited() const {
    return mInited;
  }
  bool hasMenu() const {
    return mMenu != nullptr;
  }
  bool visible() const;
  void setVisible(bool v);
  void setOptionsMode(bool v) {
    mOptionsMode = v;
  }

  // Live world readout (camera/Tomba position + current stage). Pushed each frame from
  // overlay_glue (which gates the push on Sbs::shownCore()).
  void setWorld(int x, int y, int z, unsigned stage);

  // pad_input suppresses gameplay keyboard input while this returns true.
  bool wantsKeyboard() const;

  // ---- headless driving surface (REPL `menu ...`) -------------------------------------------------
  // AGENTS MAY NOT RUN WINDOWED (docs/workspace/PROTOCOL.md) and the menu is driven by SDL keyboard events,
  // which do not exist without a window — so without these the UI is unreachable by every
  // instrument the project uses. Same class of blindness as the windowed-only init that was already
  // removed, and the same answer: the window is a SINK, not the only way in. Host UI state only.
  void selectTab(int index);
  bool sendKey(int sdl_keycode); // false = the menu did not claim that key
  void dumpMenu() const;         // enumerate every tab/pane/row + its live value

private:
  bool mInited = false;
  bool mOptionsMode = false; // stands in for the game's in-game Options menu

  SDL_Window *mWin = nullptr;
  void *mCtx = nullptr;    // Rml::Context*        (void* to keep Rml headers out of ours)
  void *mSys = nullptr;    // SystemInterface_SDL*
  void *mRender = nullptr; // RmlRenderInterfaceGpu*

  // The UI. Null when LoadDocument failed — which is exactly what hasMenu() reports.
  std::unique_ptr<psx::ui::MenuDocument> mMenu;
};
#endif // __cplusplus
#endif
