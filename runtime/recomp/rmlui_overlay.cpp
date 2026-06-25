// RmlUi (HTML/CSS) mod/debug overlay for the Tomba2 port — replaces the former Dear ImGui overlay.
// The menu layout/structure is copied from Dusklight (CC0) via soh3d's adaptation: a <window> with a
// <tab-bar> of <tab>s and a <content> of <pane>s, each pane a list of <select-button> rows (<key> +
// <value>). Navigation, activation and the value-text refresh are driven from C++ here (the same model
// as soh3d's SohRmlUi) rather than RmlUi data-bindings, because that is the proven, render-interface-
// compatible approach. Rows carry attributes the C++ reads: toggle / adjust / action.
//
// Brought up on the port's existing Vulkan device + present render pass via RmlRenderInterfaceVk
// (records into the swapchain command buffer the present pass hands it). ESC toggles the menu; quit
// lives in the menu's "Quit Game" row. The public C entry points keep the historical rmlui_overlay_*
// names so gpu_gpu.cpp / overlay_glue.cpp and pad_input.cpp's rmlui_overlay_wants_keyboard link
// unchanged. Built as C++ and linked into the otherwise-C port.

#include "rmlui_overlay.h"
#include "rmlui_render_vk.h"
#include "audio/music_list.h"   // Sound Test: native music_list_play/stop (engine/audio/)

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

extern "C" {
#include "mods.h"
}
extern "C" int g_fps60_on;                  // engine/fps60.c — the 60fps interpolation gate
extern "C" int cfg_on(const char* name);    // cfg.c
void gpu_gpu_video_status(int* native_w, int* ires, int* fbw, int* fbh, int* ww, int* wh, int* ires_cap);

// ---- static state ---------------------------------------------------------------------------------
static bool s_inited = false;
static bool s_visible = false;          // ESC opens it; starts hidden
static bool s_options_mode = false;     // stands in for the game's in-game Options menu

static SDL_Window* s_win = nullptr;
static Rml::Context* s_ctx = nullptr;
static Rml::ElementDocument* s_doc = nullptr;
static SystemInterface_SDL* s_sys = nullptr;
static RmlRenderInterfaceVk* s_render = nullptr;
static int s_active_tab = 0;

// Live world readout, pushed each frame from gpu_gpu before NewFrame.
static int s_wpos[3] = {0,0,0};
static uint32_t s_wstage = 0;
static int s_wvalid = 0;

extern "C" void rmlui_overlay_set_world(int x, int y, int z, unsigned stage) {
    s_wpos[0] = x; s_wpos[1] = y; s_wpos[2] = z; s_wstage = stage; s_wvalid = 1;
}

// ---- value formatting / live mod state per row ----------------------------------------------------
static std::string fmt_f(float v, int prec) { char b[32]; snprintf(b, sizeof b, "%.*f", prec, v); return b; }

// Build the <value> text for a row given its attribute kind/id. Returns false if unknown.
static bool row_value_text(const std::string& kind, const std::string& id, std::string& out) {
    if (kind == "toggle") {
        if (id == "aspect") {
            static const char* A[] = { "4:3", "16:9", "21:9", "Auto" };
            int a = g_mods.aspect; if (a < 0 || a > 3) a = 0; out = A[a]; return true;
        }
        if (id == "ires_auto") { out = g_mods.ires_auto ? "On" : "Off"; return true; }
        if (id == "fps60")     { out = g_fps60_on ? "On" : "Off"; return true; }
        if (id == "ssao")      { out = g_mods.ssao ? "On" : "Off"; return true; }
        if (id == "light")     { out = g_mods.light ? "On" : "Off"; return true; }
        if (id == "shadows")   { out = g_mods.shadows ? "On" : "Off"; return true; }
        if (id == "debug_ids")     { out = g_mods.debug_ids ? "On" : "Off"; return true; }
        if (id == "debug_quads")   { out = g_mods.debug_quads ? "On" : "Off"; return true; }
        if (id == "debug_objects") { out = g_mods.debug_objects ? "On" : "Off"; return true; }
    } else if (kind == "adjust") {
        if (id == "ires")            { out = std::to_string(g_mods.ires) + "x"; return true; }
        if (id == "ssao_strength")   { out = fmt_f(g_mods.ssao_strength, 2); return true; }
        if (id == "ssao_radius")     { out = fmt_f(g_mods.ssao_radius, 1); return true; }
        if (id == "ssao_bias")       { out = fmt_f(g_mods.ssao_bias, 3); return true; }
        if (id == "ssao_range")      { out = fmt_f(g_mods.ssao_range, 3); return true; }
        if (id == "light_dir_x")     { out = fmt_f(g_mods.light_dir[0], 2); return true; }
        if (id == "light_dir_y")     { out = fmt_f(g_mods.light_dir[1], 2); return true; }
        if (id == "light_dir_z")     { out = fmt_f(g_mods.light_dir[2], 2); return true; }
        if (id == "light_ambient")   { out = fmt_f(g_mods.light_ambient, 2); return true; }
        if (id == "light_diffuse")   { out = fmt_f(g_mods.light_diffuse, 2); return true; }
        if (id == "shadow_strength") { out = fmt_f(g_mods.shadow_strength, 2); return true; }
    }
    return false;
}

// Toggle (cycle bool / aspect) a row's state. mods_save() persists.
static void do_toggle(const std::string& id) {
    if (id == "aspect")          g_mods.aspect = (g_mods.aspect + 1) & 3;
    else if (id == "ires_auto")  g_mods.ires_auto = !g_mods.ires_auto;
    else if (id == "fps60")      g_fps60_on = !g_fps60_on;
    else if (id == "ssao")       g_mods.ssao = !g_mods.ssao;
    else if (id == "light")      g_mods.light = !g_mods.light;
    else if (id == "shadows")    g_mods.shadows = !g_mods.shadows;
    else if (id == "debug_ids")     { g_mods.debug_ids = !g_mods.debug_ids; return; }      // debug toggles:
    else if (id == "debug_quads")   { g_mods.debug_quads = !g_mods.debug_quads; return; }   // not persisted
    else if (id == "debug_objects") { g_mods.debug_objects = !g_mods.debug_objects; return; }
    else return;
    mods_save();
}

static void clampf(float& v, float lo, float hi) { if (v < lo) v = lo; if (v > hi) v = hi; }

// Step a numeric row by dir (+1 = Enter/A or Right, -1 = Left), clamped/wrapped. mods_save() persists.
static void do_adjust(const std::string& id, int dir) {
    if (id == "ires")                 { g_mods.ires += dir; if (g_mods.ires < 1) g_mods.ires = 3; if (g_mods.ires > 3) g_mods.ires = 1; }
    else if (id == "ssao_strength")   { g_mods.ssao_strength   += 0.05f * dir; clampf(g_mods.ssao_strength, 0.0f, 2.0f); }
    else if (id == "ssao_radius")     { g_mods.ssao_radius     += 0.5f  * dir; clampf(g_mods.ssao_radius, 1.0f, 20.0f); }
    else if (id == "ssao_bias")       { g_mods.ssao_bias       += 0.002f* dir; clampf(g_mods.ssao_bias, 0.0f, 0.1f); }
    else if (id == "ssao_range")      { g_mods.ssao_range      += 0.01f * dir; clampf(g_mods.ssao_range, 0.02f, 0.6f); }
    else if (id == "light_dir_x")     { g_mods.light_dir[0]    += 0.05f * dir; clampf(g_mods.light_dir[0], -1.0f, 1.0f); }
    else if (id == "light_dir_y")     { g_mods.light_dir[1]    += 0.05f * dir; clampf(g_mods.light_dir[1], -1.0f, 1.0f); }
    else if (id == "light_dir_z")     { g_mods.light_dir[2]    += 0.05f * dir; clampf(g_mods.light_dir[2], -1.0f, 1.0f); }
    else if (id == "light_ambient")   { g_mods.light_ambient   += 0.05f * dir; clampf(g_mods.light_ambient, 0.0f, 1.5f); }
    else if (id == "light_diffuse")   { g_mods.light_diffuse   += 0.05f * dir; clampf(g_mods.light_diffuse, 0.0f, 1.5f); }
    else if (id == "shadow_strength") { g_mods.shadow_strength += 0.05f * dir; clampf(g_mods.shadow_strength, 0.0f, 1.0f); }
    else return;
    mods_save();
}

// ---- value-text + readout refresh -----------------------------------------------------------------
static void set_row_value(Rml::Element* row) {
    std::string id, kind;
    if (!(id = row->GetAttribute<Rml::String>("toggle", "")).empty()) kind = "toggle";
    else if (!(id = row->GetAttribute<Rml::String>("adjust", "")).empty()) kind = "adjust";
    else return;
    std::string txt;
    if (!row_value_text(kind, id, txt)) return;
    if (Rml::Element* v = row->QuerySelector("value"))
        if (v->GetInnerRML() != txt) v->SetInnerRML(txt);
}

static void refresh_all_rows() {
    if (!s_doc) return;
    Rml::ElementList rows;
    s_doc->GetElementsByTagName(rows, "select-button");
    for (Rml::Element* r : rows) set_row_value(r);
}

static void refresh_readouts() {
    if (!s_doc) return;
    int nw = 320, ir = 1, fbw = 320, fbh = 240, ww = 0, wh = 0, cap = 3;
    gpu_gpu_video_status(&nw, &ir, &fbw, &fbh, &ww, &wh, &cap);
    char buf[192];
    if (Rml::Element* e = s_doc->GetElementById("video_readout")) {
        snprintf(buf, sizeof buf, "render %dx%d &middot; window %dx%d &middot; internal %dx", fbw, fbh, ww, wh, ir);
        if (e->GetInnerRML() != buf) e->SetInnerRML(buf);
    }
    if (Rml::Element* e = s_doc->GetElementById("music_readout")) {
        int np = music_list_now_playing();
        std::string txt = (np >= 0 && music_list_name(np))
                            ? (std::string("playing: ") + music_list_name(np)) : "stopped";
        if (e->GetInnerRML() != txt) e->SetInnerRML(txt);
    }
    if (Rml::Element* e = s_doc->GetElementById("world_readout")) {
        std::string txt;
        if (s_wvalid) {
            const char* sname = s_wstage == 0x8010637Cu ? "GAME" : s_wstage == 0x801062E4u ? "DEMO"
                              : s_wstage == 0x8010649Cu ? "START" : "?";
            snprintf(buf, sizeof buf, "pos X %d Y %d Z %d &middot; stage %s (0x%08X)",
                     s_wpos[0], s_wpos[1], s_wpos[2], sname, s_wstage);
            txt = buf;
        }
        if (e->GetInnerRML() != txt) e->SetInnerRML(txt);
    }
}

// ---- tab + focus navigation (mirrors soh3d's SohRmlUi) --------------------------------------------
static void apply_visibility();

static void scroll_focus_into_view() {
    if (!s_ctx) return;
    if (Rml::Element* f = s_ctx->GetFocusElement())
        f->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
}

static void focus_first_in_active_pane() {
    if (!s_doc) return;
    Rml::ElementList panes;
    s_doc->GetElementsByTagName(panes, "pane");
    if (s_active_tab < 0 || s_active_tab >= (int)panes.size()) return;
    if (Rml::Element* first = panes[s_active_tab]->QuerySelector("select-button")) {
        first->Focus();
        scroll_focus_into_view();
    }
}

static void set_active_tab(int index) {
    if (!s_ctx || !s_doc) return;
    Rml::ElementList tabs, panes;
    s_doc->GetElementsByTagName(tabs, "tab");
    s_doc->GetElementsByTagName(panes, "pane");
    const int n = (int)std::min(tabs.size(), panes.size());
    if (n == 0) return;
    if (index < 0) index = n - 1; else if (index >= n) index = 0;
    s_active_tab = index;
    for (int i = 0; i < (int)tabs.size(); i++)  tabs[i]->SetClass("selected", i == index);
    for (int i = 0; i < (int)panes.size(); i++) panes[i]->SetClass("active", i == index);
    refresh_all_rows();
    refresh_readouts();
    s_ctx->Update();
    focus_first_in_active_pane();
}

static void focus_next() { if (s_ctx) { s_ctx->ProcessKeyDown(Rml::Input::KI_TAB, 0); s_ctx->ProcessKeyUp(Rml::Input::KI_TAB, 0); scroll_focus_into_view(); } }
static void focus_prev() { if (s_ctx) { s_ctx->ProcessKeyDown(Rml::Input::KI_TAB, Rml::Input::KM_SHIFT); s_ctx->ProcessKeyUp(Rml::Input::KI_TAB, Rml::Input::KM_SHIFT); scroll_focus_into_view(); } }
static void next_tab()   { set_active_tab(s_active_tab + 1); }
static void prev_tab()   { set_active_tab(s_active_tab - 1); }

// Activate (Enter/A) or step (Left/Right with dir) the focused row.
static void activate_focused(int dir) {
    if (!s_ctx) return;
    Rml::Element* f = s_ctx->GetFocusElement();
    if (!f) return;
    std::string id;
    if (!(id = f->GetAttribute<Rml::String>("action", "")).empty()) {
        if (id == "quit") { fprintf(stderr, "[rmlui] quit from menu\n"); exit(0); }
        if (id == "close") { s_visible = false; apply_visibility(); }
        // Sound Test: action="music_<n>" plays catalogued track n; action="music_stop" stops.
        if (id.rfind("music_", 0) == 0) {
            if (id == "music_stop") music_list_stop();
            else music_list_play(atoi(id.c_str() + 6));
            refresh_readouts();
        }
        return;
    }
    if (!(id = f->GetAttribute<Rml::String>("toggle", "")).empty()) { do_toggle(id); set_row_value(f); return; }
    if (!(id = f->GetAttribute<Rml::String>("adjust", "")).empty()) { do_adjust(id, dir); set_row_value(f); return; }
}

// Click handler bound to each <tab> so a mouse click also switches tabs.
class TabClick : public Rml::EventListener {
public:
    explicit TabClick(int i) : idx(i) {}
    void ProcessEvent(Rml::Event&) override { set_active_tab(idx); }
private: int idx;
};
static std::vector<std::unique_ptr<TabClick>> s_tab_listeners;

// Click handler bound to each <select-button> so a mouse click activates it.
class RowClick : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& e) override {
        if (Rml::Element* el = e.GetCurrentElement()) { el->Focus(); activate_focused(+1); }
    }
};
static RowClick s_row_listener;

static void attach_handlers() {
    if (!s_doc) return;
    Rml::ElementList tabs;
    s_doc->GetElementsByTagName(tabs, "tab");
    s_tab_listeners.clear();
    for (int i = 0; i < (int)tabs.size(); i++) {
        auto l = std::make_unique<TabClick>(i);
        tabs[i]->AddEventListener("click", l.get());
        s_tab_listeners.push_back(std::move(l));
    }
    Rml::ElementList rows;
    s_doc->GetElementsByTagName(rows, "select-button");
    for (Rml::Element* r : rows) r->AddEventListener("click", &s_row_listener);
}

static void apply_visibility() {
    if (!s_doc) return;
    if (s_visible) {
        s_doc->Show();
        if (s_ctx) s_ctx->Update();
        set_active_tab(s_active_tab);   // shows the pane, refreshes values, focuses the first row
    } else {
        if (s_ctx) if (Rml::Element* fe = s_ctx->GetFocusElement()) fe->Blur();
        s_doc->Hide();
    }
}

// ---- init -----------------------------------------------------------------------------------------
void rmlui_overlay_init(SDL_Window* win, VkInstance inst, VkPhysicalDevice phys, uint32_t qfam,
                        VkDevice dev, VkQueue queue, VkRenderPass present_rpass,
                        uint32_t min_image_count, uint32_t image_count) {
    if (s_inited) return;
    (void)inst; (void)min_image_count;
    s_win = win;

    s_render = new RmlRenderInterfaceVk();
    uint32_t fif = image_count < 2 ? 2 : image_count;
    if (!s_render->Init(phys, dev, qfam, queue, present_rpass, fif)) {
        fprintf(stderr, "[rmlui] render interface init failed; overlay disabled\n");
        delete s_render; s_render = nullptr; return;
    }
    s_sys = new SystemInterface_SDL();
    s_sys->SetWindow(win);
    Rml::SetSystemInterface(s_sys);
    Rml::SetRenderInterface(s_render);
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
    s_ctx = Rml::CreateContext("tomba2_menu", Rml::Vector2i(ww, wh));
    if (!s_ctx) { fprintf(stderr, "[rmlui] CreateContext failed\n"); return; }
    Rml::Debugger::Initialise(s_ctx);

    s_doc = s_ctx->LoadDocument("assets/rml/menu.rml");
    if (!s_doc) {
        fprintf(stderr, "[rmlui] LoadDocument(assets/rml/menu.rml) FAILED — menu unavailable\n");
    } else {
        attach_handlers();
        refresh_all_rows();
        s_doc->Hide();   // start hidden; ESC shows it
    }

    s_inited = true;
    fprintf(stderr, "[rmlui] overlay up (ESC to toggle the menu)\n");
}

void rmlui_overlay_shutdown(void) {
    if (!s_inited) return;
    Rml::Shutdown();   // destroys contexts/documents
    s_ctx = nullptr; s_doc = nullptr;
    s_tab_listeners.clear();
    if (s_render) { s_render->Shutdown(); delete s_render; s_render = nullptr; }
    if (s_sys) { delete s_sys; s_sys = nullptr; }
    s_inited = false;
}

// ---- event pump -----------------------------------------------------------------------------------
void rmlui_overlay_event(const SDL_Event* e) {
    if (!s_inited || !e) return;
    // ESC toggles the menu (the game's old "ESC quits" was removed in gpu_gpu.cpp). In options-mode the
    // game owns visibility (Circle/Triangle), so don't fight it.
    if (!s_options_mode && e->type == SDL_KEYDOWN && e->key.repeat == 0 &&
        e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        s_visible = !s_visible; apply_visibility(); return;
    }
    if (e->type == SDL_KEYDOWN && e->key.repeat == 0 && e->key.keysym.scancode == SDL_SCANCODE_F1) {
        Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible()); return;
    }
    if (!s_visible || !s_ctx) return;

    // Menu open: arrows = nav, Enter/A = activate, Left/Right on a numeric row = step (else change tab),
    // mouse/text via the SDL platform shim.
    if (e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.sym) {
            case SDLK_DOWN:  focus_next(); return;
            case SDLK_UP:    focus_prev(); return;
            case SDLK_RIGHT: {
                Rml::Element* f = s_ctx->GetFocusElement();
                std::string id = f ? f->GetAttribute<Rml::String>("adjust", "") : std::string();
                if (!id.empty()) { do_adjust(id, +1); set_row_value(f); } else next_tab();
                return;
            }
            case SDLK_LEFT: {
                Rml::Element* f = s_ctx->GetFocusElement();
                std::string id = f ? f->GetAttribute<Rml::String>("adjust", "") : std::string();
                if (!id.empty()) { do_adjust(id, -1); set_row_value(f); } else prev_tab();
                return;
            }
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activate_focused(+1); return;
            default: { SDL_Event ev = *e; RmlSDL::InputEventHandler(s_ctx, s_win, ev); return; }
        }
    }
    // Mouse / wheel / keyup / text -> the SDL platform shim (hover, clicks, scrolling).
    SDL_Event ev = *e;
    RmlSDL::InputEventHandler(s_ctx, s_win, ev);
}

void rmlui_overlay_set_visible(int v) { s_visible = (v != 0); apply_visibility(); }
void rmlui_overlay_set_options_mode(int v) { s_options_mode = (v != 0); }

// pad_input.cpp suppresses gameplay keyboard input while this returns nonzero. The menu uses arrow/Enter
// nav (not typing), so we suppress whenever the menu is OPEN — otherwise arrow keys would also drive
// Tomba. (No text fields exist in this menu; the input/textarea checks are kept for completeness.)
extern "C" int rmlui_overlay_wants_keyboard(void) {
    if (!s_inited || !s_ctx) return 0;
    if (s_visible) return 1;
    Rml::Element* fe = s_ctx->GetFocusElement();
    if (!fe) return 0;
    const Rml::String& tag = fe->GetTagName();
    if (tag == "input") { Rml::String t = fe->GetAttribute<Rml::String>("type", "text"); if (t == "text" || t == "password") return 1; }
    if (tag == "textarea") return 1;
    return 0;
}

// ---- per-frame ------------------------------------------------------------------------------------
void rmlui_overlay_new_frame(void) {
    if (!s_inited || !s_ctx) return;
    if (s_win) {
        int ww = 0, wh = 0; SDL_GetWindowSize(s_win, &ww, &wh);
        if (ww > 0 && wh > 0) {
            Rml::Vector2i cur = s_ctx->GetDimensions();
            if (cur.x != ww || cur.y != wh) s_ctx->SetDimensions(Rml::Vector2i(ww, wh));
        }
    }
    if (s_visible) refresh_readouts();   // keep the live video/world status lines current
    s_ctx->Update();
}

// Record the menu geometry into the present pass command buffer. overlay_glue passes the FULL window
// viewport so the menu covers the whole window.
void rmlui_overlay_render_vk(VkCommandBuffer cmd, uint32_t frame_index,
                             VkViewport viewport, VkRect2D full_scissor) {
    if (!s_inited || !s_ctx || !s_render) return;
    if (!s_visible) return;
    s_render->BeginFrame(cmd, frame_index, viewport, full_scissor);
    s_ctx->Render();
    s_render->EndFrame();
}

int rmlui_overlay_inited(void) { return s_inited ? 1 : 0; }
