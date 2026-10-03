// RmlUi (HTML/CSS) mod/debug overlay — RmlUi LIFETIME ONLY.
//
// This file brings RmlUi up on the port's existing SDL_GPU device (render interface, SDL system
// interface, fonts, context, document) and routes SDL events. It knows nothing about tabs, rows,
// mods or readouts: `assets/rml/menu.rml` is handed to `psx::ui::MenuDocument` and every element,
// listener and piece of UI state lives there, as components on `psx::ui::Component` in
// `runtime/ui/`.
//
// The overlay records into the present render pass the present path hands it. ESC toggles the menu;
// F1 toggles RmlUi's own debugger. Quit lives in the menu's "Quit Game" row. One overlay per Game
// (one host window per process in practice).

#include "rmlui_overlay.h"
#include "game.h" // Game — the overlay reaches game->core for the video status
#include "rmlui_render_gpu.h"
#include <cstdlib>

#include "../ui/choice_screen.h"
#include "../ui/menu_document.h"
#include "../ui/ui_assets.h"

#include <utility>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Debugger.h>
#include <lucent/log.h>
#include <string>

// SDL platform helpers (system interface + SDL->Rml input translation) from the vendored RmlUi
// backend. RMLUI_SDL_VERSION_MAJOR is set in this TU's build flags.
#include "RmlUi_Platform_SDL.h"

void gpu_vk_video_status(Core *c, int *native_w, int *ires, int *fbw, int *fbh, int *ww, int *wh, int *ires_cap);

// ---- typed accessors for the void* handles stored on the class ----------------------------------
// The header keeps RmlUi types out (so C callers can include it). Impl-side helpers cast back.
static inline Rml::Context *ctx_(void *p) {
  return (Rml::Context *)p;
}

RmlOverlay::RmlOverlay() = default;
// Each overlay releases its OWN resources here, and never the LIBRARY's globals. `Rml::Shutdown` is
// process-global: it used to be called by whichever Game ended first, which in a process running
// several sessions at once (a title selector whose panels are live sessions) shut RmlUi down under
// the sessions still using it — the next one's `Rml::Initialise` refused, and a second `Rml::Shutdown`
// on an already-dead library segfaulted. See the lifetime notes below for where it does run.
RmlOverlay::~RmlOverlay() {
  shutdown();
}

namespace {
// How many overlays in this process are OPEN. It is only a count of open screens: the library's
// lifetime is the process's, so this reaching zero releases nothing (see below).
int liveOverlays = 0;

// The ONE renderer and ONE system interface this process's RmlUi uses, installed by the first overlay
// and never replaced.
//
// They are process-global because RML'S are: `Rml::SetRenderInterface` and `Rml::SetSystemInterface`
// publish ONE pointer for the whole library, and every context in every overlay draws through it. So
// they cannot be per-overlay in any useful sense — and per-overlay ownership was a use-after-free: a
// second overlay's document drew through the FIRST overlay's renderer, which that overlay's shutdown
// had already shut down and deleted. The renderer is also process-lifetime in the only sense that
// matters here: it is built over the presentation device, which belongs to the product and outlives
// every session.
RmlRenderInterfaceGpu *g_render = nullptr;
SystemInterface_SDL *g_sys = nullptr;

// Serial for the per-overlay context NAMES (see the CreateContext call in init). Monotonic within a
// process; a name is never reused, so a context removed and recreated cannot collide with itself.
unsigned long long nextContextSerial() {
  static unsigned long long serial = 0;
  return ++serial;
}

// Whether `Rml::Initialise` has already run in this process. NOT the same question as liveOverlays: a
// count of zero means no overlay is open right now, which for a host that rebuilds its selector
// between two titles is the normal state between two screens, and it must not read as "the library
// is down". `Rml::Initialise` runs for the first overlay of the process and never again.
bool rmlUiInitialised = false;

// Rml::Shutdown runs once, when the process is leaving — never between two overlays. `Rml::Shutdown`
// is process-global, and a product that rebuilds its UI (a title selector that returns from a game
// and builds itself again) tears every overlay down and constructs new ones inside one process. A
// refcount that shut the library down at zero made that path call `Rml::Shutdown` and then
// `Rml::Initialise` again in the same process, which segfaulted in the second picker inside RmlUi:
// the library's globals do not survive a shutdown/initialise cycle that has stack objects and a
// plugin still registered against it. RmlUi is documented as one initialise and one shutdown per
// process, so that is exactly what this does: the first overlay initialises, and the process's exit
// shuts down once. An overlay that merely ends still gives back its own interfaces (below).
void shutdownRmlUiAtExit() {
  if (rmlUiInitialised) {
    Rml::Shutdown();
    rmlUiInitialised = false;
  }
  if (g_render) {
    g_render->Shutdown();
    delete g_render;
    g_render = nullptr;
  }
  delete g_sys;
  g_sys = nullptr;
}

bool rmlUiExitHookInstalled = false;
} // namespace

namespace psx::ui {
void releaseDeviceResources() {
  shutdownRmlUiAtExit();
}
} // namespace psx::ui

// ---- init ---------------------------------------------------------------------------------------
void RmlOverlay::init(SDL_Window *win, SDL_GPUDevice *dev, SDL_GPUTextureFormat target_fmt, int sink_w, int sink_h) {
  if (mInited) {
    return;
  }
  mWin = win; // may be null — headless. See the header: the window is a sink, not a mode.

  if (!rmlUiInitialised) {
    // The process-global renderer and system interface are built and installed ONCE, by the first
    // overlay of the process. Every later overlay draws through THESE, because Rml has exactly one
    // render interface pointer for the whole library; see g_render above for why per-overlay ones
    // were a use-after-free.
    auto *render = new RmlRenderInterfaceGpu();
    if (!render->Init(dev, target_fmt)) {
      lucent::error("rmlui", "render interface init failed; overlay disabled");
      delete render;
      return;
    }
    auto *sys = new SystemInterface_SDL();
    sys->SetWindow(win);
    Rml::SetSystemInterface(sys);
    Rml::SetRenderInterface(render);
    if (!Rml::Initialise()) {
      lucent::error("rmlui", "Rml::Initialise failed; overlay disabled");
      delete render;
      delete sys;
      return;
    }
    g_render = render;
    g_sys = sys;
    rmlUiInitialised = true;
    if (!rmlUiExitHookInstalled) {
      rmlUiExitHookInstalled = true;
      std::atexit(shutdownRmlUiAtExit);
    }
  }
  mRender = g_render;
  mSys = g_sys;
  ++liveOverlays;

  // ---- assets, and a LOUD failure when they are not there ---------------------------------
  // spyro issue #52: with PSXPORT_ASSET_DIR unset every load failed and the code reported
  // success. AssetSet names the directory, names each missing file, and carries the denominator
  // (`N of M found`) so a partial load — which used to print nothing at all — is visible.
  psx::ui::AssetSet assets;
  assets.open();

  static const char *const kFonts[] = {
      "FiraSans-Regular.ttf",
      "FiraSans-Bold.ttf",
      "FiraSansCondensed-Bold.ttf",
  };
  int fonts_loaded = 0;
  for (const char *f : kFonts) {
    std::string path;
    if (!assets.require(f, path)) {
      continue;
    }
    // Present on disk but rejected by FreeType is a DIFFERENT failure from missing, and it used
    // to be indistinguishable — both produced the same silence.
    if (Rml::LoadFontFace(path.c_str())) {
      fonts_loaded++;
    } else {
      lucent::error("rmlui", "font present but REJECTED by the font engine: {}", path);
    }
  }

  std::string doc_path;
  const bool have_doc = assets.require("menu.rml", doc_path);
  assets.report();

  // The SINK's size, passed in by the caller. NOT SDL_GetWindowSize: that answers only in the
  // windowed leg, and a UI sized from a leg-dependent measurement is a UI that differs between
  // legs. Clamped rather than defaulted-to-1280x720 so a caller that passes nonsense is visible.
  if (sink_w <= 0 || sink_h <= 0) {
    lucent::warn(
        "rmlui", "init got a degenerate sink {}x{}; the menu will be laid out for it as given", sink_w, sink_h);
  }
  // Context names are per OVERLAY, because the library outlives an overlay now (see
  // shutdownRmlUiAtExit) and `Rml::CreateContext` refuses a name that is already registered. The
  // fixed names this used to pass made every overlay after the first one in a process lose its
  // context — which is every panel of a selector whose panels are their own sessions.
  mMenuCtxName = "psxport_menu#" + std::to_string(nextContextSerial());
  Rml::Context *c = Rml::CreateContext(mMenuCtxName, Rml::Vector2i(sink_w > 0 ? sink_w : 1, sink_h > 0 ? sink_h : 1));
  if (!c) {
    lucent::error("rmlui", "CreateContext({}) failed — menu unavailable", mMenuCtxName);
    return;
  }
  mCtx = c;
  Rml::Debugger::Initialise(c);

  // `mInited` means "RmlUi is up and owns resources shutdown() must free" — it is NOT the same
  // question as "is there a menu", which is `hasMenu()`. Conflating the two is what let a failed
  // LoadDocument still report success.
  mInited = true;

  Rml::ElementDocument *d = have_doc ? c->LoadDocument(doc_path.c_str()) : nullptr;
  if (!d) {
    // REPORT THE FAILURE AS THE FAILURE. This used to fall through to the "overlay up" line
    // below after its own fatal error, so a log reader was told the overlay was working while
    // ESC toggled visibility on a null document and nothing happened — the exact reason a
    // user-reported dead overlay read as a mystery rather than as a missing asset dir. An
    // instrument that reports success on its own failure path is worse than no instrument.
    lucent::error("rmlui",
                  "MENU UNAVAILABLE — the overlay is up but has NOTHING TO SHOW. {} "
                  "(fonts loaded: {}/{}; assets dir {})",
                  have_doc ? "LoadDocument(" + doc_path + ") FAILED to parse" : "menu.rml was not found",
                  fonts_loaded,
                  (int)(sizeof(kFonts) / sizeof(kFonts[0])),
                  assets.dir());
    return;
  }

  mMenu = std::make_unique<psx::ui::MenuDocument>(c, d, game);

  lucent::info("rmlui",
               "overlay up ({} sink {}x{}, {}/{} fonts, {} tabs / {} rows, ESC to toggle)",
               win ? "windowed" : "headless",
               sink_w,
               sink_h,
               fonts_loaded,
               (int)(sizeof(kFonts) / sizeof(kFonts[0])),
               mMenu->tab_count(),
               mMenu->row_count());
}

void RmlOverlay::shutdown() {
  if (!mInited) {
    return;
  }
  // ORDER MATTERS AND IT IS THE REASON THE COMPONENTS EXIST. Every listener the UI registered is
  // owned by a component, so destroying the component tree here deregisters all of them from
  // still-live elements. The old code deleted its listener objects AFTER Rml::Shutdown() had
  // already destroyed the document — survivable only because RmlUi happened to tear the elements
  // down first.
  mChoice.reset();
  mMenu.reset();
  // The overlay's own CONTEXTS go here, because the LIBRARY's are not released here (see
  // shutdownRmlUiAtExit). Rml::Shutdown used to destroy every context as a side effect, which is the
  // only reason the code below never had to: with the library outliving the overlay, an overlay that
  // kept its contexts would leave `psxport_menu` registered, and the next overlay to ask for that
  // name was refused ("context already exists") — a selector that rebuilds itself after a title
  // returns could not build its screen. A context holds documents, listeners and elements, so it is
  // unloaded first and removed second, exactly as a clean shutdown would.
  if (Rml::Context *screen = ctx_(mScreenCtx)) {
    screen->UnloadAllDocuments();
    Rml::RemoveContext(screen->GetName());
  }
  if (Rml::Context *base = ctx_(mCtx)) {
    base->UnloadAllDocuments();
    Rml::RemoveContext(base->GetName());
  }
  --liveOverlays;
  mCtx = nullptr;
  mScreenCtx = nullptr;
  // NOT deleted: the renderer and the system interface are the process's (g_render/g_sys) and are
  // released once, when the process leaves.
  mRender = nullptr;
  mSys = nullptr;
  mInited = false;
}

// ---- event pump ---------------------------------------------------------------------------------
void RmlOverlay::event(const SDL_Event *e) {
  if (!mInited || !e) {
    return;
  }
  Rml::Context *c = ctx_(mCtx);
  if (mChoice && mChoice->visible() &&
      (e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
       e->type == SDL_EVENT_MOUSE_BUTTON_UP || e->type == SDL_EVENT_MOUSE_WHEEL)) {
    SDL_Event ev = *e;
    RmlSDL::InputEventHandler(ctx_(mScreenCtx), mWin, ev);
    return;
  }
  // ESC toggles the menu (the game's old "ESC quits" was removed in gpu_vk.cpp). In options-mode the
  // game owns visibility (Circle/Triangle), so don't fight it. (SDL3 event/key field names.)
  // ESC only toggles a menu that EXISTS. Without this, a failed LoadDocument left ESC flipping
  // visibility on a null document — which showed nothing and yet made wantsKeyboard() true, so the
  // port silently swallowed every gameplay key with no menu on screen and no way to get out.
  if (!mOptionsMode && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.scancode == SDL_SCANCODE_ESCAPE) {
    if (!hasMenu()) {
      lucent::warn("rmlui", "ESC ignored: no menu document loaded (see the LoadDocument error above)");
      return;
    }
    setVisible(!mMenu->visible());
    return;
  }
  if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.scancode == SDL_SCANCODE_F1) {
    Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
    return;
  }
  if (!visible() || !c) {
    return;
  }

  // Menu open: the document takes arrows / Enter / Space; anything it does not claim falls
  // through to the SDL platform shim (hover, clicks, wheel, text).
  if (e->type == SDL_EVENT_KEY_DOWN && mMenu->handle_key((int)e->key.key)) {
    return;
  }

  SDL_Event ev = *e;
  RmlSDL::InputEventHandler(c, mWin, ev);
}

bool RmlOverlay::visible() const {
  return mMenu && mMenu->visible();
}

void RmlOverlay::setVisible(bool v) {
  if (!mMenu) {
    return;
  }
  if (v) {
    mMenu->show();
  } else {
    mMenu->hide();
  }
}

void RmlOverlay::selectTab(int index) {
  if (mMenu) {
    mMenu->select_tab(index);
  }
}
bool RmlOverlay::sendKey(int sdl_keycode) {
  return mMenu && mMenu->send_key(sdl_keycode);
}
void RmlOverlay::dumpMenu() const {
  if (mMenu) {
    mMenu->dump();
  }
}

void RmlOverlay::setWorld(int x, int y, int z, unsigned stage) {
  if (mMenu) {
    mMenu->set_world(x, y, z, stage);
  }
}

// pad_input.cpp suppresses gameplay keyboard input while this returns true. The menu uses arrow/Enter
// nav (not typing), so we suppress whenever the menu is OPEN — otherwise arrow keys would also drive
// the game. (No text fields exist in this menu; the input/textarea checks are kept for completeness.)
bool RmlOverlay::wantsKeyboard() const {
  Rml::Context *c = ctx_(mCtx);
  if (!mInited || !c || !hasMenu()) {
    return false; // no document = no UI to type into
  }
  if (mMenu->visible()) {
    return true;
  }
  Rml::Element *fe = c->GetFocusElement();
  if (!fe) {
    return false;
  }
  const Rml::String &tag = fe->GetTagName();
  if (tag == "input") {
    Rml::String t = fe->GetAttribute<Rml::String>("type", "text");
    if (t == "text" || t == "password") {
      return true;
    }
  }
  if (tag == "textarea") {
    return true;
  }
  return false;
}

// ---- per-frame ----------------------------------------------------------------------------------
void RmlOverlay::newFrame() {
  Rml::Context *c = ctx_(mCtx);
  if (!mInited || !c) {
    return;
  }
  // Track the SINK, not the window. gpu_vk_video_status reports the sink in BOTH legs (it is the
  // window size when there is a window and the configured headless present size when there is
  // not), so the menu is laid out for the same rectangle the picture is composed for either way.
  // This used to be SDL_GetWindowSize under `if (mWin)`, which meant the layout simply froze at
  // its initial size in the leg with no window.
  if (game) {
    int nw = 0, ir = 0, fbw = 0, fbh = 0, sw = 0, sh = 0, cap = 0;
    gpu_vk_video_status(&game->core, &nw, &ir, &fbw, &fbh, &sw, &sh, &cap);
    if (sw > 0 && sh > 0) {
      Rml::Vector2i cur = c->GetDimensions();
      if (cur.x != sw || cur.y != sh) {
        c->SetDimensions(Rml::Vector2i(sw, sh));
      }
    }
  }
  if (mMenu) {
    mMenu->update(); // no-op while hidden
  }
  c->Update();
  if (Rml::Context *screen = ctx_(mScreenCtx)) {
    screen->SetDimensions(c->GetDimensions());
    screen->Update();
  }
}

// Record the menu geometry into the present render pass. overlay_glue passes the FULL window size so
// the menu covers the whole window.
void RmlOverlay::recordGpu(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int win_w, int win_h) {
  Rml::Context *c = ctx_(mCtx);
  if (!mInited || !c || !mRender) {
    return;
  }
  if (!visible()) {
    return;
  }
  auto *r = (RmlRenderInterfaceGpu *)mRender;
  r->BeginFrame(cmd, rp, win_w, win_h);
  c->Render();
  r->EndFrame();
}

// ---- the choice screen -----------------------------------------------------------------------------
psx::ui::ChoiceView *RmlOverlay::showChoiceScreen(psx::ui::ChoiceContent content) {
  if (!mInited) {
    lucent::error("rmlui", "showChoiceScreen: the overlay is not up, so there is nowhere to show a screen");
    return nullptr;
  }
  mChoice.reset();
  if (mScreenCtx) {
    ctx_(mScreenCtx)->UnloadAllDocuments();
  }
  Rml::Context *base = ctx_(mCtx);
  if (!mScreenCtx) {
    mScreenCtx = Rml::CreateContext(mMenuCtxName + "_screen", base->GetDimensions());
    if (!mScreenCtx) {
      lucent::error("rmlui", "CreateContext({}_screen) failed — no choice screen", mMenuCtxName);
      return nullptr;
    }
  }
  psx::ui::AssetSet assets;
  assets.open();
  std::string path;
  if (!assets.require("choice.rml", path)) {
    assets.report();
    return nullptr;
  }
  Rml::ElementDocument *doc = ctx_(mScreenCtx)->LoadDocument(path.c_str());
  if (!doc) {
    lucent::error("rmlui", "LoadDocument({}) FAILED to parse — no choice screen", path);
    return nullptr;
  }
  mChoice = std::make_unique<psx::ui::ChoiceScreen>(ctx_(mScreenCtx), doc, std::move(content));
  mChoice->show();
  return mChoice.get();
}

psx::ui::ChoiceView *RmlOverlay::choiceScreen() const {
  return mChoice.get();
}

void RmlOverlay::hideChoiceScreen() {
  if (mChoice) {
    mChoice->hide();
    mChoice.reset();
    ctx_(mScreenCtx)->UnloadAllDocuments();
  }
}

void RmlOverlay::recordScreenGpu(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int w, int h) {
  Rml::Context *c = ctx_(mScreenCtx);
  if (!mInited || !c || !mRender || !mChoice || !mChoice->visible()) {
    return;
  }
  auto *r = (RmlRenderInterfaceGpu *)mRender;
  r->BeginFrame(cmd, rp, w, h);
  c->Render();
  r->EndFrame();
}
