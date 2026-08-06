// RmlUi (HTML/CSS) mod/debug overlay for the Tomba2 port — replaces the former Dear ImGui overlay.
// The menu layout/structure is copied from Dusklight (CC0) via soh3d's adaptation: a <window> with a
// <tab-bar> of <tab>s and a <content> of <pane>s, each pane a list of <select-button> rows (<key> +
// <value>). Navigation, activation and the value-text refresh are driven from C++ here (the same model
// as soh3d's SohRmlUi) rather than RmlUi data-bindings, because that is the proven, render-interface-
// compatible approach. Rows carry attributes the C++ reads: toggle / adjust / action.
//
// Brought up on the port's existing SDL_GPU device via RmlRenderInterfaceGpu (records into the present
// render pass the present path hands it). ESC toggles the menu; quit lives in the menu's "Quit Game"
// row. All UI state lives on `class RmlOverlay`, embedded on Game as `game->rml_overlay` — the
// back-pointer to Game is wired in Game(). The Sound Test / music HUD reach the game's MusicList
// through the audioNowPlayingName / audioSoundTestPlay GameHooks (the overlay names no game type).
// One overlay per Game (one host window per process in practice).

#include "rmlui_overlay.h"
#include "rmlui_render_gpu.h"
#include "game.h"                // Game — the overlay reaches game->core / game->mods / game->core.hooks

#include <RmlUi/Core.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Debugger.h>
#include "cfg.h"
#include "config_vars.h"                  // cv_asset_dir — PSXPORT_ASSET_DIR asset-base resolution
#include "rml_text.h"                     // set_text()'s DATA -> markup boundary; RML_TEXT_SEP
#include <lucent/log.h>
#include <string>

// SDL platform helpers (system interface + SDL->Rml input translation) from the vendored RmlUi
// backend. RMLUI_SDL_VERSION_MAJOR is set in this TU's build flags.
#include "RmlUi_Platform_SDL.h"

// Resolve a framework asset path (fonts + menu.rml, which ship in <framework>/assets/rml/) against
// PSXPORT_ASSET_DIR. These are DISK-loaded (unlike the SPIR-V shaders, which are embedded), so the
// consumer must tell the framework where its assets live: after the repo split they moved into the
// psxport submodule, so a game whose runtime cwd is its own root sets PSXPORT_ASSET_DIR to the dir
// containing assets/ (e.g. `external/psxport`). Unset (default) keeps the historical cwd-relative
// behaviour, so a framework-root launch still works with no config.
static std::string rml_asset(const char* rel) {
    const std::string& base = psx::config::cv_asset_dir.get();
    return base.empty() ? std::string(rel) : base + "/" + rel;
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

#include "mods.h"
#include "game_hooks_opt.h"   // OPTIONAL hooks are never called directly — see that header
// g_fps60_on retired — read game->mods.fps60 (mods.h; #included above)
extern "C" int cfg_on(const char* name);    // cfg.c
void gpu_vk_video_status(Core* c, int* native_w, int* ires, int* fbw, int* fbh, int* ww, int* wh, int* ires_cap);

// ---- typed accessors for the void* handles stored on the class ----------------------------------
// The header keeps RmlUi types out (so C callers can include it). Impl-side helpers cast back.
static inline Rml::Context*         ctx_(void* p)  { return (Rml::Context*)p; }
static inline Rml::ElementDocument* doc_(void* p)  { return (Rml::ElementDocument*)p; }

// ---- the ONE place overlay text enters the DOM ---------------------------------------------------
// Every readout and row value is DATA — numbers, a stage name, a track title — and `SetInnerRML`
// PARSES ITS ARGUMENT AS MARKUP, so data must be encoded on the way in. Going through one boundary
// is what keeps two failure modes structurally impossible rather than merely absent today:
//
//   * a value can never be read as markup. The readouts used to be snprintf'd straight into
//     SetInnerRML with a `&middot;` separator, and RmlUi decodes only `&lt; &gt; &amp; &quot;` plus
//     numeric references — so the user saw the literal text "&middot;" (see rml_text.h).
//   * the "don't rewrite unless it changed" guard actually fires. `GetInnerRML()` returns
//     `EncodeRml(text)`, so comparing it against a RAW string containing `& < > "` never matched
//     and the element was reparsed and relaid-out every single frame. Comparing ENCODED against
//     ENCODED is the same string on both sides.
//
// tests/test_rml_text_encoding.cpp asserts this file holds exactly one raw inner-RML call site —
// the one below. Add readouts by calling set_text, never by reaching for that setter again.
static void set_text(Rml::Element* el, std::string_view text) {
    if (!el) return;
    const std::string markup = rml_text_markup(text);
    if (el->GetInnerRML() == markup) return;
    el->SetInnerRML(markup);
    // Report what the DOM NOW HOLDS, not what we sent — DecodeRml(GetInnerRML()) is literally the
    // character sequence RmlUi will hand to the font. That distinction is the whole point: the
    // "&middot;" bug was invisible to any diagnostic that echoed the string we composed, because
    // the string we composed was fine and the DOM's interpretation of it was not. Channel `rmlui`.
    const Rml::String& id = el->GetId();
    lucent::debug("rmlui", "text {} = \"{}\"", id.empty() ? el->GetTagName() : "#" + id,
                  Rml::StringUtilities::DecodeRml(el->GetInnerRML()));
}

// ---- per-element event listeners (implementation-only classes) ----------------------------------
// Each listener holds a pointer back to its owning RmlOverlay so the callback routes UI state
// updates through the correct instance (no static instance() lookup).
class TabClick : public Rml::EventListener {
public:
    TabClick(RmlOverlay* owner, int i) : owner_(owner), idx_(i) {}
    void ProcessEvent(Rml::Event&) override { owner_->setActiveTab(idx_); }
private:
    RmlOverlay* owner_;
    int         idx_;
};

class RowClick : public Rml::EventListener {
public:
    explicit RowClick(RmlOverlay* owner) : owner_(owner) {}
    void ProcessEvent(Rml::Event& e) override {
        if (Rml::Element* el = e.GetCurrentElement()) {
            el->Focus();
            owner_->activateFocused(+1);
        }
    }
private:
    RmlOverlay* owner_;
};

using TabListVec = std::vector<std::unique_ptr<TabClick>>;

// ---- value formatting / live mod state per row --------------------------------------------------
static std::string fmt_f(float v, int prec) { char b[32]; snprintf(b, sizeof b, "%.*f", prec, v); return b; }

// Build the <value> text for a row given its attribute kind/id. Returns false if unknown.
static bool row_value_text(Mods& m, const std::string& kind, const std::string& id, std::string& out) {
    if (kind == "toggle") {
        if (id == "aspect") {
            static const char* A[] = { "Vanilla", "16:9", "21:9", "Auto" };
            int a = m.aspect; if (a < 0 || a > 3) a = 0; out = A[a]; return true;
        }
        if (id == "ires") {
            // Merged resolution selector: 0 = Auto, 1..4 = Vanilla(1x)/X2/X3/X4.
            static const char* R[] = { "Auto", "Vanilla", "X2", "X3", "X4" };
            int r = m.ires; if (r < 0 || r > 4) r = 1; out = R[r]; return true;
        }
        if (id == "fps60")     { out = m.fps60 ? "On" : "Off"; return true; }
        if (id == "ssao")      { out = m.ssao ? "On" : "Off"; return true; }
        if (id == "light")     { out = m.light ? "On" : "Off"; return true; }
        if (id == "shadows")   { out = m.shadows ? "On" : "Off"; return true; }
        if (id == "debug_ids")     { out = m.debug_ids ? "On" : "Off"; return true; }
        if (id == "debug_quads")   { out = m.debug_quads ? "On" : "Off"; return true; }
        if (id == "debug_objects") { out = m.debug_objects ? "On" : "Off"; return true; }
    } else if (kind == "adjust") {
        if (id == "ssao_strength")   { out = fmt_f(m.ssao_strength, 2); return true; }
        if (id == "ssao_radius")     { out = fmt_f(m.ssao_radius, 1); return true; }
        if (id == "ssao_bias")       { out = fmt_f(m.ssao_bias, 3); return true; }
        if (id == "ssao_range")      { out = fmt_f(m.ssao_range, 3); return true; }
        if (id == "light_dir_x")     { out = fmt_f(m.light_dir[0], 2); return true; }
        if (id == "light_dir_y")     { out = fmt_f(m.light_dir[1], 2); return true; }
        if (id == "light_dir_z")     { out = fmt_f(m.light_dir[2], 2); return true; }
        if (id == "light_ambient")   { out = fmt_f(m.light_ambient, 2); return true; }
        if (id == "light_diffuse")   { out = fmt_f(m.light_diffuse, 2); return true; }
        if (id == "shadow_strength") { out = fmt_f(m.shadow_strength, 2); return true; }
    }
    return false;
}

// Toggle (cycle bool / aspect) a row's state. mods_save() persists.
static void do_toggle(Mods& m, const std::string& id) {
    if (id == "aspect")          m.aspect = (m.aspect + 1) & 3;
    // Merged resolution cycle: Vanilla(1)->X2(2)->X3(3)->X4(4)->Auto(0)->Vanilla. (0=Auto convention.)
    else if (id == "ires")       { m.ires += 1; if (m.ires > 4) m.ires = 0; }
    else if (id == "fps60")      m.fps60 = !m.fps60;
    else if (id == "ssao")       m.ssao = !m.ssao;
    else if (id == "light")      m.light = !m.light;
    else if (id == "shadows")    m.shadows = !m.shadows;
    else if (id == "debug_ids")     { m.debug_ids = !m.debug_ids; return; }      // debug toggles:
    else if (id == "debug_quads")   { m.debug_quads = !m.debug_quads; return; }   // not persisted
    else if (id == "debug_objects") { m.debug_objects = !m.debug_objects; return; }
    else return;
    m.save();
}

static void clampf(float& v, float lo, float hi) { if (v < lo) v = lo; if (v > hi) v = hi; }

// Step a numeric row by dir (+1 = Enter/A or Right, -1 = Left), clamped/wrapped. mods_save() persists.
static void do_adjust(Mods& m, const std::string& id, int dir) {
    if (id == "ssao_strength")        { m.ssao_strength   += 0.05f * dir; clampf(m.ssao_strength, 0.0f, 2.0f); }
    else if (id == "ssao_radius")     { m.ssao_radius     += 0.5f  * dir; clampf(m.ssao_radius, 1.0f, 20.0f); }
    else if (id == "ssao_bias")       { m.ssao_bias       += 0.002f* dir; clampf(m.ssao_bias, 0.0f, 0.1f); }
    else if (id == "ssao_range")      { m.ssao_range      += 0.01f * dir; clampf(m.ssao_range, 0.02f, 0.6f); }
    else if (id == "light_dir_x")     { m.light_dir[0]    += 0.05f * dir; clampf(m.light_dir[0], -1.0f, 1.0f); }
    else if (id == "light_dir_y")     { m.light_dir[1]    += 0.05f * dir; clampf(m.light_dir[1], -1.0f, 1.0f); }
    else if (id == "light_dir_z")     { m.light_dir[2]    += 0.05f * dir; clampf(m.light_dir[2], -1.0f, 1.0f); }
    else if (id == "light_ambient")   { m.light_ambient   += 0.05f * dir; clampf(m.light_ambient, 0.0f, 1.5f); }
    else if (id == "light_diffuse")   { m.light_diffuse   += 0.05f * dir; clampf(m.light_diffuse, 0.0f, 1.5f); }
    else if (id == "shadow_strength") { m.shadow_strength += 0.05f * dir; clampf(m.shadow_strength, 0.0f, 1.0f); }
    else return;
    m.save();
}

// ---- dev AREA WARP row (Debug tab) --------------------------------------------------------------
// The framework knows nothing about areas: how many there are, what they are called, and whether a warp
// is legal right now all come from the game through GameHooks (game/core/dev_areas.cpp). Arming reuses
// the SAME path the REPL `warp` command uses — Repl::warpArmed/warpDest — so there is one warp
// mechanism, not a second one behind a button.
int RmlOverlay::warpAreaCount() const {
    if (!game || !game->core.hooks || !game->core.hooks->devAreaCount) return 0;
    return game->core.hooks->devAreaCount(&game->core);
}

// Row label: "12 - Ghost Pig boss" when the game has a sourced name, plain "Area 12" when it does not.
// An unnamed area is shown as its index rather than a guess (docs/areas.md: an index is a fact, a name
// is a claim that needs a source).
std::string RmlOverlay::warpAreaLabel(int area) const {
    const char* nm = (game && game->core.hooks && game->core.hooks->devAreaName)
                   ? game->core.hooks->devAreaName(&game->core, area) : "";
    char b[96];
    if (nm && *nm) snprintf(b, sizeof b, "%d - %s", area, nm);
    else           snprintf(b, sizeof b, "Area %d", area);
    return b;
}

void RmlOverlay::adjustWarpArea(int dir) {
    const int n = warpAreaCount();
    if (n <= 0) return;
    mWarpArea = (mWarpArea + dir) % n;
    if (mWarpArea < 0) mWarpArea += n;   // wrap both ways, so Left from 0 reaches the last area
}

// Arm the warp. Returns a status line for the readout: the game decides whether a warp is legal (it is
// only legal from the field, because the warp runs the game's OWN door transition — fade, teardown, CD
// settle, reload — and that needs a running field machine to carry it out).
std::string RmlOverlay::armWarp() {
    if (!game || !game->core.hooks) return "warp unavailable";
    if (warpAreaCount() <= 0) return "warp unavailable";
    if (game->core.hooks->devWarpAllowed && !game->core.hooks->devWarpAllowed(&game->core))
        return "not in the field - reach gameplay first";
    game->repl.warpDest  = (uint32_t)mWarpArea;
    game->repl.warpSub   = 0;
    game->repl.warpArmed = 1;
    lucent::info("rmlui", "warp: armed area {} from the menu", mWarpArea);
    return "warping to " + warpAreaLabel(mWarpArea);
}

// ---- value-text + readout refresh ---------------------------------------------------------------
void RmlOverlay::setRowValue(void* rowRaw) {
    Rml::Element* row = (Rml::Element*)rowRaw;
    std::string id, kind;
    if (!(id = row->GetAttribute<Rml::String>("toggle", "")).empty()) kind = "toggle";
    else if (!(id = row->GetAttribute<Rml::String>("adjust", "")).empty()) kind = "adjust";
    else return;
    std::string txt;
    if (!game) return;
    if (kind == "adjust" && id == "warp_area") txt = warpAreaLabel(mWarpArea);
    else if (!row_value_text(game->mods, kind, id, txt)) return;
    set_text(row->QuerySelector("value"), txt);
}

void RmlOverlay::refreshAllRows() {
    Rml::ElementDocument* d = doc_(mDoc);
    if (!d) return;
    Rml::ElementList rows;
    d->GetElementsByTagName(rows, "select-button");
    for (Rml::Element* r : rows) setRowValue(r);
}

// Write the warp status line into the Debug tab's readout div (empty id = the div is absent, e.g. a
// game whose menu.rml has no warp section — the overlay must not require it).
void RmlOverlay::setWarpReadout(const std::string& txt) {
    Rml::ElementDocument* d = doc_(mDoc);
    if (!d) return;
    set_text(d->GetElementById("warp_readout"), txt);
}

void RmlOverlay::refreshReadouts() {
    Rml::ElementDocument* d = doc_(mDoc);
    if (!d) return;
    int nw = 320, ir = 1, fbw = 320, fbh = 240, ww = 0, wh = 0, cap = 4;
    gpu_vk_video_status(game ? &game->core : nullptr, &nw, &ir, &fbw, &fbh, &ww, &wh, &cap);
    char buf[192];

    // RML_TEXT_SEP is a real U+00B7 character in the TEXT, not an entity: these strings are data,
    // and `set_text` encodes them so nothing in them is ever parsed as markup (see rml_text.h).
    snprintf(buf, sizeof buf, "render %dx%d" RML_TEXT_SEP "window %dx%d" RML_TEXT_SEP "internal %dx",
             fbw, fbh, ww, wh, ir);
    set_text(d->GetElementById("video_readout"), buf);

    std::string music = "stopped";
    if (game) {
        const char* nm = game_audio_now_playing_name(&game->core, game->core.hooks);
        if (nm) music = std::string("playing: ") + nm;
    }
    set_text(d->GetElementById("music_readout"), music);

    std::string world;
    if (mWvalid) {
        const char* sname = mWstage == 0x8010637Cu ? "GAME" : mWstage == 0x801062E4u ? "DEMO"
                          : mWstage == 0x8010649Cu ? "START" : "?";
        snprintf(buf, sizeof buf, "pos X %d Y %d Z %d" RML_TEXT_SEP "stage %s (0x%08X)",
                 mWpos[0], mWpos[1], mWpos[2], sname, mWstage);
        world = buf;
    }
    set_text(d->GetElementById("world_readout"), world);
}

// ---- tab + focus navigation (mirrors soh3d's SohRmlUi) ------------------------------------------
void RmlOverlay::scrollFocusIntoView() {
    Rml::Context* c = ctx_(mCtx);
    if (!c) return;
    if (Rml::Element* f = c->GetFocusElement())
        f->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
}

void RmlOverlay::focusFirstInActivePane() {
    Rml::ElementDocument* d = doc_(mDoc);
    if (!d) return;
    Rml::ElementList panes;
    d->GetElementsByTagName(panes, "pane");
    if (mActiveTab < 0 || mActiveTab >= (int)panes.size()) return;
    if (Rml::Element* first = panes[mActiveTab]->QuerySelector("select-button")) {
        first->Focus();
        scrollFocusIntoView();
    }
}

void RmlOverlay::setActiveTab(int index) {
    Rml::Context* c = ctx_(mCtx);
    Rml::ElementDocument* d = doc_(mDoc);
    if (!c || !d) return;
    Rml::ElementList tabs, panes;
    d->GetElementsByTagName(tabs, "tab");
    d->GetElementsByTagName(panes, "pane");
    const int n = (int)std::min(tabs.size(), panes.size());
    if (n == 0) return;
    if (index < 0) index = n - 1; else if (index >= n) index = 0;
    mActiveTab = index;
    for (int i = 0; i < (int)tabs.size(); i++)  tabs[i]->SetClass("selected", i == index);
    for (int i = 0; i < (int)panes.size(); i++) panes[i]->SetClass("active", i == index);
    refreshAllRows();
    refreshReadouts();
    c->Update();
    focusFirstInActivePane();
}

void RmlOverlay::focusNext() {
    Rml::Context* c = ctx_(mCtx);
    if (!c) return;
    c->ProcessKeyDown(Rml::Input::KI_TAB, 0);
    c->ProcessKeyUp  (Rml::Input::KI_TAB, 0);
    scrollFocusIntoView();
}
void RmlOverlay::focusPrev() {
    Rml::Context* c = ctx_(mCtx);
    if (!c) return;
    c->ProcessKeyDown(Rml::Input::KI_TAB, Rml::Input::KM_SHIFT);
    c->ProcessKeyUp  (Rml::Input::KI_TAB, Rml::Input::KM_SHIFT);
    scrollFocusIntoView();
}

// Activate (Enter/A) or step (Left/Right with dir) the focused row.
void RmlOverlay::activateFocused(int dir) {
    Rml::Context* c = ctx_(mCtx);
    if (!c) return;
    Rml::Element* f = c->GetFocusElement();
    if (!f) return;
    std::string id;
    if (!(id = f->GetAttribute<Rml::String>("action", "")).empty()) {
        if (id == "quit")  { lucent::info("rmlui", "quit from menu"); exit(0); }
        if (id == "close") { mVisible = false; applyVisibility(); }
        // Sound Test: action="music_<n>" plays catalogued track n; action="music_stop" stops.
        if (id == "warp_go") { setWarpReadout(armWarp()); return; }
        if (id.rfind("music_", 0) == 0 && game) {
            if (id == "music_stop") game_audio_sound_test_play(&game->core, game->core.hooks, -1);
            else                    game_audio_sound_test_play(&game->core, game->core.hooks, atoi(id.c_str() + 6));
            refreshReadouts();
        }
        return;
    }
    if (!(id = f->GetAttribute<Rml::String>("toggle", "")).empty()) { if (game) { do_toggle(game->mods, id); setRowValue(f); } return; }
    if (!(id = f->GetAttribute<Rml::String>("adjust", "")).empty()) {
        if (id == "warp_area") { adjustWarpArea(dir); setRowValue(f); return; }
        if (game) { do_adjust(game->mods, id, dir); setRowValue(f); }
        return;
    }
}

void RmlOverlay::attachHandlers() {
    Rml::ElementDocument* d = doc_(mDoc);
    if (!d) return;
    if (!mTabListeners) mTabListeners = new TabListVec();
    if (!mRowListener)  mRowListener  = new RowClick(this);
    TabListVec* tabs_vec = (TabListVec*)mTabListeners;
    RowClick*   row_l    = (RowClick*)mRowListener;

    Rml::ElementList tabs;
    d->GetElementsByTagName(tabs, "tab");
    tabs_vec->clear();
    for (int i = 0; i < (int)tabs.size(); i++) {
        auto l = std::make_unique<TabClick>(this, i);
        tabs[i]->AddEventListener("click", l.get());
        tabs_vec->push_back(std::move(l));
    }
    Rml::ElementList rows;
    d->GetElementsByTagName(rows, "select-button");
    for (Rml::Element* r : rows) r->AddEventListener("click", row_l);
}

void RmlOverlay::applyVisibility() {
    Rml::ElementDocument* d = doc_(mDoc);
    if (!d) return;
    if (mVisible) {
        d->Show();
        if (Rml::Context* c = ctx_(mCtx)) c->Update();
        setActiveTab(mActiveTab);   // shows the pane, refreshes values, focuses the first row
    } else {
        if (Rml::Context* c = ctx_(mCtx))
            if (Rml::Element* fe = c->GetFocusElement()) fe->Blur();
        d->Hide();
    }
}

// ---- init ---------------------------------------------------------------------------------------
void RmlOverlay::init(SDL_Window* win, SDL_GPUDevice* dev, SDL_GPUTextureFormat target_fmt,
                      int sink_w, int sink_h) {
    if (mInited) return;
    mWin = win;   // may be null — headless. See the header: the window is a sink, not a mode.

    auto* render = new RmlRenderInterfaceGpu();
    if (!render->Init(dev, target_fmt)) {
        lucent::error("rmlui", "render interface init failed; overlay disabled");
        delete render; return;
    }
    mRender = render;

    auto* sys = new SystemInterface_SDL();
    sys->SetWindow(win);
    mSys = sys;
    Rml::SetSystemInterface(sys);
    Rml::SetRenderInterface(render);
    if (!Rml::Initialise()) { lucent::error("rmlui", "Rml::Initialise failed; overlay disabled"); return; }

    const char* fonts[] = {
        "assets/rml/FiraSans-Regular.ttf",
        "assets/rml/FiraSans-Bold.ttf",
        "assets/rml/FiraSansCondensed-Bold.ttf",
    };
    int loaded = 0;
    for (const char* f : fonts) if (Rml::LoadFontFace(rml_asset(f).c_str())) loaded++;
    if (!loaded) lucent::warn("rmlui", "WARNING: no fonts loaded (assets/rml/*.ttf missing; set PSXPORT_ASSET_DIR to the dir containing assets/)");

    // The SINK's size, passed in by the caller. NOT SDL_GetWindowSize: that answers only in the
    // windowed leg, and a UI sized from a leg-dependent measurement is a UI that differs between
    // legs. Clamped rather than defaulted-to-1280x720 so a caller that passes nonsense is visible.
    if (sink_w <= 0 || sink_h <= 0) {
        lucent::warn("rmlui", "init got a degenerate sink {}x{}; the menu will be laid out for it as given", sink_w, sink_h);
    }
    Rml::Context* c = Rml::CreateContext("tomba2_menu", Rml::Vector2i(sink_w > 0 ? sink_w : 1, sink_h > 0 ? sink_h : 1));
    if (!c) { lucent::error("rmlui", "CreateContext failed — menu unavailable"); return; }
    mCtx = c;
    Rml::Debugger::Initialise(c);

    const std::string doc_path = rml_asset("assets/rml/menu.rml");
    Rml::ElementDocument* d = c->LoadDocument(doc_path.c_str());
    // `mInited` means "RmlUi is up and owns resources shutdown() must free" — it is NOT the same
    // question as "is there a menu", which is `hasMenu()`. Conflating the two is what let a failed
    // LoadDocument still report success.
    mInited = true;

    if (!d) {
        // REPORT THE FAILURE AS THE FAILURE. This used to fall through to the "overlay up" line
        // below after its own fatal error, so a log reader was told the overlay was working while
        // ESC toggled visibility on a null document and nothing happened — the exact reason a
        // user-reported dead overlay read as a mystery rather than as a missing asset dir. An
        // instrument that reports success on its own failure path is worse than no instrument.
        lucent::error("rmlui", "LoadDocument({}) FAILED — MENU UNAVAILABLE, the overlay is up but "
                               "has NOTHING TO SHOW (set PSXPORT_ASSET_DIR to the dir containing "
                               "assets/; fonts loaded: {}/3)", doc_path, loaded);
        return;
    }
    mDoc = d;
    attachHandlers();
    refreshAllRows();
    d->Hide();   // start hidden; ESC shows it

    lucent::info("rmlui", "overlay up ({} sink {}x{}, {}/3 fonts, ESC to toggle the menu)",
                 win ? "windowed" : "headless", sink_w, sink_h, loaded);
}

void RmlOverlay::shutdown() {
    if (!mInited) return;
    Rml::Shutdown();   // destroys contexts/documents
    mCtx = nullptr; mDoc = nullptr;
    if (mTabListeners) { delete (TabListVec*)mTabListeners; mTabListeners = nullptr; }
    if (mRowListener)  { delete (RowClick*)mRowListener;    mRowListener  = nullptr; }
    if (mRender) { ((RmlRenderInterfaceGpu*)mRender)->Shutdown(); delete (RmlRenderInterfaceGpu*)mRender; mRender = nullptr; }
    if (mSys)    { delete (SystemInterface_SDL*)mSys; mSys = nullptr; }
    mInited = false;
}

// ---- event pump ---------------------------------------------------------------------------------
void RmlOverlay::event(const SDL_Event* e) {
    if (!mInited || !e) return;
    Rml::Context* c = ctx_(mCtx);
    // ESC toggles the menu (the game's old "ESC quits" was removed in gpu_vk.cpp). In options-mode the
    // game owns visibility (Circle/Triangle), so don't fight it. (SDL3 event/key field names.)
    // ESC only toggles a menu that EXISTS. Without this, a failed LoadDocument left ESC flipping
    // mVisible on a null document — which showed nothing and yet made wantsKeyboard() true, so the
    // port silently swallowed every gameplay key with no menu on screen and no way to get out.
    if (!mOptionsMode && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
        e->key.scancode == SDL_SCANCODE_ESCAPE) {
        if (!hasMenu()) {
            lucent::warn("rmlui", "ESC ignored: no menu document loaded (see the LoadDocument error above)");
            return;
        }
        mVisible = !mVisible; applyVisibility(); return;
    }
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.scancode == SDL_SCANCODE_F1) {
        Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible()); return;
    }
    if (!mVisible || !c) return;

    // Menu open: arrows = nav, Enter/A = activate, Left/Right on a numeric row = step (else change tab),
    // mouse/text via the SDL platform shim.
    if (e->type == SDL_EVENT_KEY_DOWN) {
        switch (e->key.key) {
            case SDLK_DOWN:  focusNext(); return;
            case SDLK_UP:    focusPrev(); return;
            case SDLK_RIGHT: {
                Rml::Element* f = c->GetFocusElement();
                std::string id = f ? f->GetAttribute<Rml::String>("adjust", "") : std::string();
                if (id == "warp_area")  { adjustWarpArea(+1); setRowValue(f); }
                else if (!id.empty()) { if (game) do_adjust(game->mods, id, +1); setRowValue(f); } else nextTab();
                return;
            }
            case SDLK_LEFT: {
                Rml::Element* f = c->GetFocusElement();
                std::string id = f ? f->GetAttribute<Rml::String>("adjust", "") : std::string();
                if (id == "warp_area")  { adjustWarpArea(-1); setRowValue(f); }
                else if (!id.empty()) { if (game) do_adjust(game->mods, id, -1); setRowValue(f); } else prevTab();
                return;
            }
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activateFocused(+1); return;
            default: { SDL_Event ev = *e; RmlSDL::InputEventHandler(c, mWin, ev); return; }
        }
    }
    // Mouse / wheel / keyup / text -> the SDL platform shim (hover, clicks, scrolling).
    SDL_Event ev = *e;
    RmlSDL::InputEventHandler(c, mWin, ev);
}

void RmlOverlay::setVisible(bool v)                            { mVisible = v; applyVisibility(); }
void RmlOverlay::setWorld(int x, int y, int z, unsigned stage) { mWpos[0]=x; mWpos[1]=y; mWpos[2]=z; mWstage=stage; mWvalid=true; }

// pad_input.cpp suppresses gameplay keyboard input while this returns true. The menu uses arrow/Enter
// nav (not typing), so we suppress whenever the menu is OPEN — otherwise arrow keys would also drive
// Tomba. (No text fields exist in this menu; the input/textarea checks are kept for completeness.)
bool RmlOverlay::wantsKeyboard() const {
    Rml::Context* c = ctx_(mCtx);
    if (!mInited || !c || !hasMenu()) return false;   // no document = no UI to type into
    if (mVisible) return true;
    Rml::Element* fe = c->GetFocusElement();
    if (!fe) return false;
    const Rml::String& tag = fe->GetTagName();
    if (tag == "input") { Rml::String t = fe->GetAttribute<Rml::String>("type", "text"); if (t == "text" || t == "password") return true; }
    if (tag == "textarea") return true;
    return false;
}

// ---- per-frame ----------------------------------------------------------------------------------
void RmlOverlay::newFrame() {
    Rml::Context* c = ctx_(mCtx);
    if (!mInited || !c) return;
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
            if (cur.x != sw || cur.y != sh) c->SetDimensions(Rml::Vector2i(sw, sh));
        }
    }
    if (mVisible) refreshReadouts();   // keep the live video/world status lines current
    c->Update();
}

// Record the menu geometry into the present render pass. overlay_glue passes the FULL window size so
// the menu covers the whole window.
void RmlOverlay::recordGpu(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* rp, int win_w, int win_h) {
    Rml::Context* c = ctx_(mCtx);
    if (!mInited || !c || !mRender) return;
    if (!mVisible) return;
    auto* r = (RmlRenderInterfaceGpu*)mRender;
    r->BeginFrame(cmd, rp, win_w, win_h);
    c->Render();
    r->EndFrame();
}

