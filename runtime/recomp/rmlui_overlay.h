// class RmlOverlay — the RmlUi (HTML/CSS) mod/debug overlay + world-readout HUD.
//
// One singleton per process (one SDL window, one HTML document, one RmlUi context). All state
// — SDL/Rml handles, tab selection, visibility, world-readout cache, per-element event
// listeners — lives on the instance. See rmlui_overlay.cpp for the design commentary.
//
// Not per-Core — process-scoped by design (single window, single UI). Cross-core visibility is
// handled at the CALL SITE (overlay_glue gates the world-coord push on Sbs::shownCore()).
//
// Legacy free-function API (`rmlui_overlay_*`) is kept for existing callers (overlay_glue,
// pad_input, menu) — each is a one-line bridge to the singleton method.
#ifndef PSXPORT_RMLUI_OVERLAY_H
#define PSXPORT_RMLUI_OVERLAY_H
#include <SDL3/SDL.h>

#ifdef __cplusplus
class RmlOverlay {
public:
  static RmlOverlay& instance();

  // Bring RmlUi up on the port's existing SDL_GPU device, for the given swapchain colour format.
  // Call once after the device + swapchain exist (windowed only). No-op if already inited.
  void init(SDL_Window* win, SDL_GPUDevice* dev, SDL_GPUTextureFormat swap_fmt);
  void shutdown();

  // Feed every SDL event (ESC toggles the menu; F1 debugger). Safe if not inited.
  void event(const SDL_Event* e);
  // CPU-side per-frame update (RmlUi context + live-mod row refresh). Safe if not inited.
  void newFrame();
  // Record the menu geometry into the present render pass. No-op when menu is hidden / not inited.
  void recordGpu(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* rp, int win_w, int win_h);

  bool inited() const { return mInited; }
  void setVisible(bool v);
  void setOptionsMode(bool v) { mOptionsMode = v; }

  // Live world readout (camera/Tomba position + current stage). Pushed each frame from
  // overlay_glue (which gates the push on Sbs::shownCore() so only one core drives the HUD).
  void setWorld(int x, int y, int z, unsigned stage);

  // pad_input suppresses gameplay keyboard input while this returns true.
  bool wantsKeyboard() const;

  // Called by the per-element event listeners (defined in the .cpp) — routed through the singleton
  // rather than kept as free-function statics so all UI state stays on the class.
  void setActiveTab(int index);
  void activateFocused(int dir);

private:
  RmlOverlay() = default;

  void setRowValue(void* /*Rml::Element*/ row);   // void* to keep Rml out of the header
  void refreshAllRows();
  void refreshReadouts();
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

  // World-position HUD, pushed each frame by overlay_glue (gated on Sbs::shownCore()).
  int      mWpos[3] = {0, 0, 0};
  uint32_t mWstage  = 0;
  bool     mWvalid  = false;

  // Per-element event listeners (opaque here — concrete types are the TabClick / RowClick classes
  // defined at file scope in the .cpp so they can reach RmlUi types).
  void* mTabListeners = nullptr;   // std::vector<std::unique_ptr<TabClick>>*
  void* mRowListener  = nullptr;   // RowClick* (single shared listener, all rows bind to it)
};
#endif // __cplusplus

// Legacy free-function API — thin bridges to `RmlOverlay::instance()` methods.
#ifdef __cplusplus
extern "C" {
#endif
void rmlui_overlay_init(SDL_Window* win, SDL_GPUDevice* dev, SDL_GPUTextureFormat swap_fmt);
void rmlui_overlay_shutdown(void);
void rmlui_overlay_event(const SDL_Event* e);
void rmlui_overlay_new_frame(void);
void rmlui_overlay_record_gpu(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* rp, int win_w, int win_h);
int  rmlui_overlay_inited(void);
void rmlui_overlay_set_visible(int v);
void rmlui_overlay_set_options_mode(int v);
void rmlui_overlay_set_world(int x, int y, int z, unsigned stage);
int  rmlui_overlay_wants_keyboard(void);
#ifdef __cplusplus
}
#endif
#endif
