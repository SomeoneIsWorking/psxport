// class RmlOverlay — the RmlUi (HTML/CSS) mod/debug overlay + world-readout HUD.
//
// One per Game (`c->game->rml_overlay.method()`). Owns the SDL/Rml handles, tab selection,
// visibility, world-readout cache, and per-element event listeners. The back-pointer to Game is
// wired in Game(); the Sound Test rows reach the game's MusicList through the audio GameHooks.
//
// In SBS with two Games only one Game's overlay wins the SDL window; the other's `mInited` stays
// false and all methods no-op. In standalone the sole Game owns the UI.
#ifndef PSXPORT_RMLUI_OVERLAY_H
#define PSXPORT_RMLUI_OVERLAY_H
#include <string>
#include <SDL3/SDL.h>
#include <cstdint>

#ifdef __cplusplus
class Game;

class RmlOverlay {
public:
  Game* game = nullptr;   // back-pointer wired by Game()

  // Bring RmlUi up on the port's existing SDL_GPU device. Call once after the device exists.
  // No-op if already inited.
  //
  // THE WINDOW IS OPTIONAL AND `win` MAY BE NULL. Whether the UI EXISTS must not depend on whether
  // there is a window (coord/PROTOCOL.md: "the window is an output sink, not a mode") — this used
  // to be called under `if (!s_headless)`, which made every overlay failure invisible to every
  // headless instrument, and that is precisely why a user-reported dead overlay could not be
  // diagnosed without taking the user's screen. `win` is used ONLY for input translation and the
  // SDL system interface; it is never the source of a size.
  //
  // `sink_w`/`sink_h` are the SINK's size, measured by the caller and passed in explicitly — never
  // re-derived here from SDL_GetWindowSize, which exists in one leg only. `target_fmt` is the
  // colour format of the pass the overlay will record into (swapchain windowed, present image
  // headless).
  void init(SDL_Window* win, SDL_GPUDevice* dev, SDL_GPUTextureFormat target_fmt,
            int sink_w, int sink_h);
  void shutdown();

  // Feed every SDL event (ESC toggles the menu; F1 debugger). Safe if not inited.
  void event(const SDL_Event* e);
  // CPU-side per-frame update (RmlUi context + live-mod row refresh). Safe if not inited.
  void newFrame();
  // Record the menu geometry into the present render pass. No-op when menu is hidden / not inited.
  void recordGpu(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* rp, int win_w, int win_h);

  // `inited()` = RmlUi is up and owns resources. It is NOT "there is a menu": LoadDocument can fail
  // (typically a missing PSXPORT_ASSET_DIR) and leave a live context with no document. Ask
  // `hasMenu()` before anything that assumes a UI exists — conflating the two is what let a failed
  // asset load still log "overlay up".
  bool inited() const { return mInited; }
  bool hasMenu() const { return mDoc != nullptr; }
  bool visible() const { return mVisible; }
  void setVisible(bool v);
  void setOptionsMode(bool v) { mOptionsMode = v; }

  // Live world readout (camera/Tomba position + current stage). Pushed each frame from
  // overlay_glue (which gates the push on Sbs::shownCore()).
  void setWorld(int x, int y, int z, unsigned stage);

  // pad_input suppresses gameplay keyboard input while this returns true.
  bool wantsKeyboard() const;

  // Called by the per-element event listeners (defined in the .cpp) — routed through this
  // instance rather than kept as free-function statics so all UI state stays on the class.
  void setActiveTab(int index);
  void activateFocused(int dir);

private:
  void setRowValue(void* /*Rml::Element*/ row);   // void* to keep Rml out of the header
  void refreshAllRows();
  void refreshReadouts();
  // Dev AREA WARP row (Debug tab). The area count, the names and the "is a warp legal now" test all
  // come from the game via GameHooks — the framework holds only the current selection. Arming reuses
  // Repl::warpArmed/warpDest, the same path the REPL `warp` command takes, so there is ONE warp
  // mechanism rather than a second one behind a button.
  int         warpAreaCount() const;
  std::string warpAreaLabel(int area) const;
  void        adjustWarpArea(int dir);
  std::string armWarp();
  void        setWarpReadout(const std::string& txt);
  void scrollFocusIntoView();
  void focusFirstInActivePane();
  void focusNext();
  void focusPrev();
  void nextTab() { setActiveTab(mActiveTab + 1); }
  void prevTab() { setActiveTab(mActiveTab - 1); }
  void applyVisibility();
  void attachHandlers();

  bool mInited      = false;
  bool mVisible     = false;    // ESC opens it; starts hidden
  bool mOptionsMode = false;    // stands in for the game's in-game Options menu

  SDL_Window* mWin = nullptr;
  void* mCtx    = nullptr;      // Rml::Context*        (void* to keep Rml headers out of ours)
  void* mDoc    = nullptr;      // Rml::ElementDocument*
  void* mSys    = nullptr;      // SystemInterface_SDL*
  void* mRender = nullptr;      // RmlRenderInterfaceGpu*
  int   mActiveTab = 0;
  int   mWarpArea  = 0;         // dev-warp destination selection; transient, deliberately not persisted

  // World-position HUD, pushed each frame by overlay_glue.
  int      mWpos[3] = {0, 0, 0};
  uint32_t mWstage  = 0;
  bool     mWvalid  = false;

  // Per-element event listeners (opaque here — concrete types defined at file scope in the .cpp).
  void* mTabListeners = nullptr;   // std::vector<std::unique_ptr<TabClick>>*
  void* mRowListener  = nullptr;   // RowClick*
};
#endif // __cplusplus
#endif
