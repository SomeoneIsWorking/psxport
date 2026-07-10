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
// back-pointer to Game is wired in Game() so this reaches `game->music_list` for the Sound Test.
// One overlay per Game (one host window per process in practice).

#include "rmlui_overlay.h"
#include "rmlui_render_gpu.h"
#include "game.h"                // Game — owns music_list; overlay reaches game->music_list

#include <RmlUi/Core.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Debugger.h>

// SDL platform helpers (system interface + SDL->Rml input translation) from the vendored RmlUi
// backend. RMLUI_SDL_VERSION_MAJOR is set in this TU's build flags.
#include "RmlUi_Platform_SDL.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

#include "mods.h"
// g_fps60_on retired — read game->mods.fps60 (mods.h; #included above)
extern "C" int cfg_on(const char* name);    // cfg.c
void gpu_gpu_video_status(Core* c, int* native_w, int* ires, int* fbw, int* fbh, int* ww, int* wh, int* ires_cap);

// ---- typed accessors for the void* handles stored on the class ----------------------------------
// The header keeps RmlUi types out (so C callers can include it). Impl-side helpers cast back.
static inline Rml::Context*         ctx_(void* p)  { return (Rml::Context*)p; }
static inline Rml::ElementDocument* doc_(void* p)  { return (Rml::ElementDocument*)p; }

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

// ---- value-text + readout refresh ---------------------------------------------------------------
void RmlOverlay::setRowValue(void* rowRaw) {
    Rml::Element* row = (Rml::Element*)rowRaw;
    std::string id, kind;
    if (!(id = row->GetAttribute<Rml::String>("toggle", "")).empty()) kind = "toggle";
    else if (!(id = row->GetAttribute<Rml::String>("adjust", "")).empty()) kind = "adjust";
    else return;
    std::string txt;
    if (!game) return;
    if (!row_value_text(game->mods, kind, id, txt)) return;
    if (Rml::Element* v = row->QuerySelector("value"))
        if (v->GetInnerRML() != txt) v->SetInnerRML(txt);
}

void RmlOverlay::refreshAllRows() {
    Rml::ElementDocument* d = doc_(mDoc);
    if (!d) return;
    Rml::ElementList rows;
    d->GetElementsByTagName(rows, "select-button");
    for (Rml::Element* r : rows) setRowValue(r);
}

void RmlOverlay::refreshReadouts() {
    Rml::ElementDocument* d = doc_(mDoc);
    if (!d) return;
    int nw = 320, ir = 1, fbw = 320, fbh = 240, ww = 0, wh = 0, cap = 4;
    gpu_gpu_video_status(game ? &game->core : nullptr, &nw, &ir, &fbw, &fbh, &ww, &wh, &cap);
    char buf[192];
    if (Rml::Element* e = d->GetElementById("video_readout")) {
        snprintf(buf, sizeof buf, "render %dx%d &middot; window %dx%d &middot; internal %dx", fbw, fbh, ww, wh, ir);
        if (e->GetInnerRML() != buf) e->SetInnerRML(buf);
    }
    if (Rml::Element* e = d->GetElementById("music_readout")) {
        std::string txt = "stopped";
        if (game) {
            int np = game->music_list.nowPlaying();
            if (np >= 0 && game->music_list.name(np))
                txt = std::string("playing: ") + game->music_list.name(np);
        }
        if (e->GetInnerRML() != txt) e->SetInnerRML(txt);
    }
    if (Rml::Element* e = d->GetElementById("world_readout")) {
        std::string txt;
        if (mWvalid) {
            const char* sname = mWstage == 0x8010637Cu ? "GAME" : mWstage == 0x801062E4u ? "DEMO"
                              : mWstage == 0x8010649Cu ? "START" : "?";
            snprintf(buf, sizeof buf, "pos X %d Y %d Z %d &middot; stage %s (0x%08X)",
                     mWpos[0], mWpos[1], mWpos[2], sname, mWstage);
            txt = buf;
        }
        if (e->GetInnerRML() != txt) e->SetInnerRML(txt);
    }
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
        if (id == "quit")  { fprintf(stderr, "[rmlui] quit from menu\n"); exit(0); }
        if (id == "close") { mVisible = false; applyVisibility(); }
        // Sound Test: action="music_<n>" plays catalogued track n; action="music_stop" stops.
        if (id.rfind("music_", 0) == 0 && game) {
            if (id == "music_stop") game->music_list.stop();
            else                    game->music_list.play(atoi(id.c_str() + 6));
            refreshReadouts();
        }
        return;
    }
    if (!(id = f->GetAttribute<Rml::String>("toggle", "")).empty()) { if (game) { do_toggle(game->mods, id); setRowValue(f); } return; }
    if (!(id = f->GetAttribute<Rml::String>("adjust", "")).empty()) { if (game) { do_adjust(game->mods, id, dir); setRowValue(f); } return; }
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
void RmlOverlay::init(SDL_Window* win, SDL_GPUDevice* dev, SDL_GPUTextureFormat swap_fmt) {
    if (mInited) return;
    mWin = win;

    auto* render = new RmlRenderInterfaceGpu();
    if (!render->Init(dev, swap_fmt)) {
        fprintf(stderr, "[rmlui] render interface init failed; overlay disabled\n");
        delete render; return;
    }
    mRender = render;

    auto* sys = new SystemInterface_SDL();
    sys->SetWindow(win);
    mSys = sys;
    Rml::SetSystemInterface(sys);
    Rml::SetRenderInterface(render);
    if (!Rml::Initialise()) { fprintf(stderr, "[rmlui] Rml::Initialise failed; overlay disabled\n"); return; }

    const char* fonts[] = {
        "assets/rml/FiraSans-Regular.ttf",
        "assets/rml/FiraSans-Bold.ttf",
        "assets/rml/FiraSansCondensed-Bold.ttf",
    };
    int loaded = 0;
    for (const char* f : fonts) if (Rml::LoadFontFace(f)) loaded++;
    if (!loaded) fprintf(stderr, "[rmlui] WARNING: no fonts loaded (assets/rml/*.ttf missing)\n");

    int ww = 0, wh = 0; SDL_GetWindowSize(win, &ww, &wh);
    if (ww <= 0) ww = 1280; if (wh <= 0) wh = 720;
    Rml::Context* c = Rml::CreateContext("tomba2_menu", Rml::Vector2i(ww, wh));
    if (!c) { fprintf(stderr, "[rmlui] CreateContext failed\n"); return; }
    mCtx = c;
    Rml::Debugger::Initialise(c);

    Rml::ElementDocument* d = c->LoadDocument("assets/rml/menu.rml");
    if (!d) {
        fprintf(stderr, "[rmlui] LoadDocument(assets/rml/menu.rml) FAILED — menu unavailable\n");
    } else {
        mDoc = d;
        attachHandlers();
        refreshAllRows();
        d->Hide();   // start hidden; ESC shows it
    }

    mInited = true;
    fprintf(stderr, "[rmlui] overlay up (ESC to toggle the menu)\n");
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
    // ESC toggles the menu (the game's old "ESC quits" was removed in gpu_gpu.cpp). In options-mode the
    // game owns visibility (Circle/Triangle), so don't fight it. (SDL3 event/key field names.)
    if (!mOptionsMode && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
        e->key.scancode == SDL_SCANCODE_ESCAPE) {
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
                if (!id.empty()) { if (game) do_adjust(game->mods, id, +1); setRowValue(f); } else nextTab();
                return;
            }
            case SDLK_LEFT: {
                Rml::Element* f = c->GetFocusElement();
                std::string id = f ? f->GetAttribute<Rml::String>("adjust", "") : std::string();
                if (!id.empty()) { if (game) do_adjust(game->mods, id, -1); setRowValue(f); } else prevTab();
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
    if (!mInited || !c) return false;
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
    if (mWin) {
        int ww = 0, wh = 0; SDL_GetWindowSize(mWin, &ww, &wh);
        if (ww > 0 && wh > 0) {
            Rml::Vector2i cur = c->GetDimensions();
            if (cur.x != ww || cur.y != wh) c->SetDimensions(Rml::Vector2i(ww, wh));
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

