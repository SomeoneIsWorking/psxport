#include "menu_document.h"

#include "game.h"
#include "game_hooks_opt.h"   // OPTIONAL hooks are never called directly — see that header

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <lucent/log.h>

#include <SDL3/SDL.h>

#include <cstdlib>

namespace psx::ui {
namespace {

// Attribute names the document uses to say what a row is wired to. Kept together so the set is
// visible in one place rather than as five string literals scattered through the ladders.
constexpr const char* kAttrAction = "action";
constexpr const char* kAttrToggle = "toggle";
constexpr const char* kAttrAdjust = "adjust";

// The one adjust row whose model is not `Mods`.
constexpr const char* kWarpAreaId = "warp_area";

}  // namespace

MenuDocument::MenuDocument(Rml::Context* ctx, Rml::ElementDocument* doc, Game* game)
    : Component(doc), mCtx(ctx), mDoc(doc), mGame(game), mWarp(game) {
    if (!mDoc) return;

    // Readouts first: the initial tab selection at the bottom of this constructor refreshes the
    // whole tree, so the readouts must already exist by then.
    mReadouts = &adopt<MenuReadouts>(mDoc, mGame);

    if (Rml::Element* bar = mDoc->QuerySelector("tab-bar"))
        mTabBar = &adopt<MenuTabBar>(bar, [this](int index) { on_tab_selected(index); });

    Rml::ElementList panes;
    mDoc->GetElementsByTagName(panes, "pane");
    // The cast is explicit because RowBuilder is a PRIVATE base: the conversion is accessible here
    // but not inside make_unique, which is where adopt<>() actually performs it.
    RowBuilder& builder = *this;
    for (Rml::Element* p : panes) mPanes.push_back(&adopt<MenuPane>(p, builder));

    // THE MISMATCH THAT USED TO BE SILENT. The old code indexed tabs and panes by one integer and
    // clamped with std::min, so a document with six tabs and five panes simply lost the sixth tab's
    // page and said nothing — a tab that opens onto nothing reads as a rendering bug. It is a
    // document error and it is named as one.
    if (mTabBar && mTabBar->count() != (int)mPanes.size()) {
        lucent::error("rmlui",
                      "menu document MISMATCH: {} <tab> vs {} <pane> — they are paired by position, "
                      "so {} of them have no counterpart and will do nothing",
                      mTabBar->count(), (int)mPanes.size(),
                      mTabBar->count() > (int)mPanes.size() ? mTabBar->count() - (int)mPanes.size()
                                                            : (int)mPanes.size() - mTabBar->count());
    }

    lucent::info("rmlui", "menu built: {} tabs, {} panes, {} rows, {} of 4 readouts{}",
                 tab_count(), pane_count(), row_count(), readout_count(),
                 mUnknownRows ? " — SEE THE UNRECOGNISED ROW ERRORS ABOVE" : "");

    if (mTabBar) mTabBar->select(0);   // stamps :selected on tab 0 and shows pane 0
    mDoc->Hide();                      // start hidden; ESC shows it
}

MenuDocument::~MenuDocument() = default;

void MenuDocument::dump() const {
    // Counts FIRST, so the enumeration below always arrives with its denominator attached. A dump
    // that printed rows without saying how many it expected could not tell "the Sound tab is empty"
    // from "I stopped enumerating".
    lucent::info("rmlui", "MENU: {} tabs, {} panes, {} rows, {} of 4 readouts, {} unrecognised row(s)",
                 tab_count(), pane_count(), row_count(), readout_count(), mUnknownRows);
    for (int i = 0; i < (int)mPanes.size(); i++) {
        lucent::info("rmlui", "  pane {}{}{}: {} row(s)", i,
                     mTabBar && mTabBar->active() == i ? " [SELECTED]" : "",
                     mPanes[i]->shown() ? " [shown]" : "", mPanes[i]->row_count());
        for (const MenuRow* r : mPanes[i]->rows())
            lucent::info("rmlui", "    {}", r->describe());
    }
}

int MenuDocument::row_count() const {
    int n = 0;
    for (const MenuPane* p : mPanes) n += p->row_count();
    return n;
}

// ---- RowBuilder ------------------------------------------------------------------------------------
std::unique_ptr<RowBinding> MenuDocument::bind_row(Rml::Element* row) {
    if (!row) return nullptr;

    // Order matters and matches the old ladder: an `action` row is a one-shot button even if it
    // also carries a `toggle`/`adjust` attribute.
    std::string id = row->GetAttribute<Rml::String>(kAttrAction, "");
    if (!id.empty()) return make_action_binding([this, id] { run_action(id); });

    id = row->GetAttribute<Rml::String>(kAttrToggle, "");
    if (!id.empty()) {
        if (!ModRowModel::knows(RowKind::Toggle, id)) {
            mUnknownRows++;
            lucent::error("rmlui",
                          "menu row toggle=\"{}\" is UNRECOGNISED — the row will display its "
                          "authored placeholder and do nothing when activated (known toggles: {})",
                          id, ModRowModel::toggle_count());
            return nullptr;
        }
        return make_mod_toggle_binding(mGame ? &mGame->mods : nullptr, id);
    }

    id = row->GetAttribute<Rml::String>(kAttrAdjust, "");
    if (!id.empty()) {
        if (id == kWarpAreaId) return make_warp_area_binding(&mWarp);
        if (!ModRowModel::knows(RowKind::Adjust, id)) {
            mUnknownRows++;
            lucent::error("rmlui",
                          "menu row adjust=\"{}\" is UNRECOGNISED — the row will display its "
                          "authored placeholder and do nothing when activated (known adjusts: {})",
                          id, ModRowModel::adjust_count());
            return nullptr;
        }
        return make_mod_adjust_binding(mGame ? &mGame->mods : nullptr, id);
    }

    // A <select-button> with none of the three attributes is focusable but inert. That is a
    // document error too — it takes a place in the navigation order and does nothing.
    mUnknownRows++;
    lucent::error("rmlui", "menu row has no action/toggle/adjust attribute — it is focusable but "
                           "inert, and it still occupies a slot in the Up/Down order");
    return nullptr;
}

void MenuDocument::on_row_clicked(MenuRow& row) {
    // Match the keyboard path exactly: focus the row, then activate it with dir = +1.
    row.focus();
    row.step(+1);
}

// ---- actions ---------------------------------------------------------------------------------------
void MenuDocument::run_action(const std::string& id) {
    if (id == "quit") {
        lucent::info("rmlui", "quit from menu");
        exit(0);
    }
    if (id == "close") { hide(); return; }
    if (id == "warp_go") {
        if (mReadouts) mReadouts->set_warp_status(mWarp.arm());
        return;
    }
    // Sound Test: action="music_<n>" plays catalogued track n; action="music_stop" stops.
    if (id.rfind("music_", 0) == 0) {
        if (!mGame) return;
        if (id == "music_stop") game_audio_sound_test_play(&mGame->core, mGame->core.hooks, -1);
        else game_audio_sound_test_play(&mGame->core, mGame->core.hooks, atoi(id.c_str() + 6));
        if (mReadouts) mReadouts->update();
        return;
    }
    // Reported rather than ignored: an action id nobody handles is a dead button, and the old code
    // returned silently from exactly here.
    lucent::error("rmlui", "menu row action=\"{}\" is UNRECOGNISED — the button does nothing", id);
}

// ---- visibility + per-frame -------------------------------------------------------------------------
void MenuDocument::show() {
    if (!mDoc) return;
    mVisible = true;
    mDoc->Show();
    if (mCtx) mCtx->Update();
    // Re-select the current tab: shows its pane, refreshes every value, focuses the first row.
    if (mTabBar) mTabBar->select(mTabBar->active());
}

void MenuDocument::hide() {
    if (!mDoc) return;
    mVisible = false;
    if (mCtx)
        if (Rml::Element* fe = mCtx->GetFocusElement()) fe->Blur();
    mDoc->Hide();
}

void MenuDocument::update() {
    if (!mVisible) return;
    Component::update();   // readouts + the shown pane's rows; hidden panes return early
}

// ---- navigation -------------------------------------------------------------------------------------
// The tab bar's callback — NOT a way to change tab. Everything that wants a different tab calls
// `mTabBar->select()`, which owns the wrap and the selected state and then calls this. Having two
// entry points for "change tab" is what would make the re-entry ambiguous.
void MenuDocument::on_tab_selected(int index) {
    if (!mDoc) return;
    for (int i = 0; i < (int)mPanes.size(); i++) mPanes[i]->set_shown(i == index);
    Component::update();   // refresh the newly shown pane's rows and the readouts before focusing
    if (mCtx) mCtx->Update();
    if (index >= 0 && index < (int)mPanes.size()) mPanes[index]->focus_first_row();
}

MenuRow* MenuDocument::focused_row() const {
    if (!mCtx) return nullptr;
    Rml::Element* fe = mCtx->GetFocusElement();
    if (!fe) return nullptr;
    for (MenuPane* p : mPanes)
        if (MenuRow* r = p->row_containing(fe)) return r;
    return nullptr;
}

void MenuDocument::focus_step(int dir) {
    if (!mCtx) return;
    // RmlUi owns focus order (driven by the `tab-index: auto` RCSS property, menu.rcss:141), so
    // Down/Up are its own TAB / Shift+TAB navigation rather than an index we maintain in parallel.
    const int mods = dir < 0 ? Rml::Input::KM_SHIFT : 0;
    mCtx->ProcessKeyDown(Rml::Input::KI_TAB, mods);
    mCtx->ProcessKeyUp(Rml::Input::KI_TAB, mods);
    if (Rml::Element* f = mCtx->GetFocusElement())
        f->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
}

void MenuDocument::activate_focused(int dir) {
    if (MenuRow* r = focused_row()) r->step(dir);
}

bool MenuDocument::handle_key(int sdl_keycode) {
    if (!mVisible) return false;
    switch (sdl_keycode) {
        case SDLK_DOWN:  focus_step(+1); return true;
        case SDLK_UP:    focus_step(-1); return true;
        case SDLK_RIGHT:
        case SDLK_LEFT: {
            const int dir = sdl_keycode == SDLK_RIGHT ? +1 : -1;
            MenuRow* r = focused_row();
            // A row that steps with arrows consumes them; anything else lets them change TAB. That
            // is why a toggle row's Left/Right moves between tabs — deliberate, and preserved.
            if (r && r->steps_with_arrows()) r->step(dir);
            else if (mTabBar) mTabBar->select(mTabBar->active() + dir);
            return true;
        }
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: activate_focused(+1); return true;
        default: return false;
    }
}

}  // namespace psx::ui
