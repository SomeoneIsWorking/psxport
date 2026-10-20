// rmlui_overlay.cpp — RmlUi context, fonts and menu document per Game, and SDL event routing. ESC toggles
// the menu; F1 toggles RmlUi's debugger.
#include "rmlui_overlay.h"
#include "game.h"
#include "render.h"
#include "rml_text.h"
#include "rmlui_library.h"
#include "rmlui_render_gpu.h"

#include "../ui/menu_document.h"
#include "../ui/ui_assets.h"

#include <utility>

#include <bug_report/form.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Debugger.h>
#include <lucent/log.h>
#include <string>

#include "RmlUi_Platform_SDL.h"

namespace {

inline Rml::Context *ctx_(void *p) {
  return static_cast<Rml::Context *>(p);
}

} // namespace

RmlOverlay::RmlOverlay() = default;

RmlOverlay::~RmlOverlay() {
  shutdown();
}

void RmlOverlay::init(psx::ui::RmlUiLibrary &library, psx::present::PixelSize sink) {
  if (mInited || !library.ready()) {
    return;
  }
  mLibrary = &library;
  const int sink_w = sink.width;
  const int sink_h = sink.height;

  psx::ui::AssetSet assets;
  assets.open();

  constexpr const char *const kFonts[] = {
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
    if (Rml::LoadFontFace(path.c_str())) {
      fonts_loaded++;
    } else {
      lucent::error("rmlui", "font present but REJECTED by the font engine: {}", path);
    }
  }

  std::string doc_path;
  const bool have_doc = assets.require("menu.rml", doc_path);
  assets.report();

  if (sink_w <= 0 || sink_h <= 0) {
    lucent::warn(
        "rmlui", "init got a degenerate sink {}x{}; the menu will be laid out for it as given", sink_w, sink_h);
  }
  const std::string contextName = library.nextContextName("psxport_menu");
  Rml::Context *c = Rml::CreateContext(contextName, Rml::Vector2i(sink_w > 0 ? sink_w : 1, sink_h > 0 ? sink_h : 1));
  if (!c) {
    lucent::error("rmlui", "CreateContext({}) failed, menu unavailable", contextName);
    return;
  }
  mCtx = c;
  Rml::Debugger::Initialise(c);

  mInited = true;

  Rml::ElementDocument *d = have_doc ? c->LoadDocument(doc_path.c_str()) : nullptr;
  if (!d) {
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
               library.window() != nullptr ? "windowed" : "headless",
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
  // The components deregister their listeners from still-live elements before the context goes.
  mBugReport.reset();
  mMenu.reset();
  if (Rml::Context *base = ctx_(mCtx)) {
    base->UnloadAllDocuments();
    Rml::RemoveContext(base->GetName());
  }
  mCtx = nullptr;
  mLibrary = nullptr;
  mInited = false;
}

// ---- event pump ---------------------------------------------------------------------------------
void RmlOverlay::event(const SDL_Event *e) {
  if (!mInited || !e) {
    return;
  }
  Rml::Context *c = ctx_(mCtx);
  if (mBugReport && c) {
    // The report form is modal: it takes every event, ESC (its cancel) included.
    SDL_Event ev = *e;
    RmlSDL::InputEventHandler(c, mLibrary->window(), ev);
    return;
  }
  // In options-mode the game owns visibility; ESC toggles only a menu that exists.
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
  if (e->type == SDL_EVENT_KEY_DOWN && mMenu->handle_key(e->key.key)) {
    return;
  }

  SDL_Event ev = *e;
  RmlSDL::InputEventHandler(c, mLibrary->window(), ev);
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
bool RmlOverlay::sendKey(SDL_Keycode key) {
  return mMenu && mMenu->handle_key(key);
}
void RmlOverlay::dumpMenu() const {
  if (mMenu) {
    mMenu->dump();
  }
}

// pad_input.cpp suppresses gameplay keyboard input while this returns true. The menu uses arrow/Enter
// nav (not typing), so we suppress whenever the menu is OPEN — otherwise arrow keys would also drive
// the game. (No text fields exist in this menu; the input/textarea checks are kept for completeness.)
bool RmlOverlay::wantsKeyboard() const {
  if (mBugReport) {
    return true;
  }
  Rml::Context *c = ctx_(mCtx);
  if (!mInited || !c || !hasMenu()) {
    return false; // no document = no UI to type into
  }
  return mMenu->visible() || typingText();
}

bool RmlOverlay::typingText() const {
  if (mBugReport) {
    return true;
  }
  Rml::Context *c = ctx_(mCtx);
  if (!mInited || !c) {
    return false;
  }
  Rml::Element *fe = c->GetFocusElement();
  if (!fe) {
    return false;
  }
  const Rml::String &tag = fe->GetTagName();
  if (tag == "input") {
    Rml::String t = psx::ui::rmlAttributeText(*fe, "type", "text");
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
  // Laid out for the sink, which both legs have.
  if (game) {
    const psx::render::VideoStatus video = psx::render::videoStatus(game->core);
    const int sw = video.sinkWidth;
    const int sh = video.sinkHeight;
    if (sw > 0 && sh > 0) {
      Rml::Vector2i cur = c->GetDimensions();
      if (cur.x != sw || cur.y != sh) {
        c->SetDimensions(Rml::Vector2i(sw, sh));
      }
    }
  }
  // dp follows the window's display scale, so the UI keeps its physical size on a scaled display.
  // Read each frame rather than on SDL's scale-change event, which a hidden menu never receives.
  SDL_Window *window = mLibrary->window();
  c->SetDensityIndependentPixelRatio(window != nullptr ? SDL_GetWindowDisplayScale(window) : 1.0f);
  if (mMenu) {
    mMenu->update(); // no-op while hidden
  }
  c->Update();
}

// The full window size, so the menu covers the whole window.
void RmlOverlay::recordGpu(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int win_w, int win_h) {
  Rml::Context *c = ctx_(mCtx);
  if (!mInited || !c) {
    return;
  }
  if (!visible() && !mBugReport) {
    return;
  }
  RmlRenderInterfaceGpu &r = mLibrary->renderer();
  r.BeginFrame(cmd, rp, win_w, win_h);
  c->Render();
  r.EndFrame();
}

// ---- the bug report form -----------------------------------------------------------------------------
bug_report::Form *RmlOverlay::showBugReport(const bug_report::Draft &draft) {
  Rml::Context *c = ctx_(mCtx);
  if (!mInited || !c) {
    return nullptr;
  }
  mBugReport.reset();
  setVisible(false); // the form is modal; the menu would only sit behind it
  mBugReport = std::make_unique<bug_report::Form>(*c, draft, bug_report::FormOptions{.fontFamily = "Fira Sans"});
  if (!mBugReport->loaded()) {
    lucent::error("rmlui", "the bug report form did not load");
  }
  return mBugReport.get();
}

void RmlOverlay::closeBugReport() {
  mBugReport.reset();
}
