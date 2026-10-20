#include "menu_document.h"

#include "game.h"
#include "mod_row_model.h"
#include "number_text.h"
#include "rml_text.h"
#include "ui_component.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <lucent/log.h>

#include <SDL3/SDL.h>

#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

namespace psx::ui {
namespace {

// The attributes that say what a row is wired to.
constexpr const char *kAttrAction = "action";
constexpr const char *kAttrToggle = "toggle";
constexpr const char *kAttrAdjust = "adjust";

// The Debug tab's adjust rows.
constexpr const char *kWarpAreaId = "warp_area";
constexpr const char *kDevCommandIds[] = {"warp_entry", "flag_index", "flag_value"};

// A title's declared settings: adjust rows with this id prefix, added under `#title_settings`.
constexpr std::string_view kTitleSettingPrefix = "title:";
constexpr const char *kTitleSettingsHostId = "title_settings";

} // namespace

MenuDocument::MenuDocument(Rml::Context *ctx, Rml::ElementDocument *doc, Game *game)
    : Component(doc), mCtx(ctx), mDoc(doc), mGame(game), mDevCommands(game), mWarp(game, &mDevCommands) {
  if (!mDoc) {
    return;
  }

  add_title_setting_rows();

  // Before the first tab selection below, which refreshes them.
  mReadouts = &adopt<MenuReadouts>(mDoc, mGame);

  if (Rml::Element *bar = mDoc->QuerySelector("tab-bar")) {
    mTabBar = &adopt<MenuTabBar>(bar, [this](int index) {
      on_tab_selected(index);
    });
  }

  Rml::ElementList panes;
  mDoc->GetElementsByTagName(panes, "pane");
  // RowBuilder is a private base, so the conversion happens here rather than inside adopt<>().
  RowBuilder &builder = *this;
  for (Rml::Element *p : panes) {
    mPanes.push_back(&adopt<MenuPane>(p, builder));
  }

  // Tabs and panes pair by position.
  if (mTabBar && mTabBar->count() != (int)mPanes.size()) {
    lucent::error("rmlui",
                  "menu document mismatch: {} <tab> vs {} <pane>; {} of them pair with nothing",
                  mTabBar->count(),
                  (int)mPanes.size(),
                  mTabBar->count() > (int)mPanes.size() ? mTabBar->count() - (int)mPanes.size()
                                                        : (int)mPanes.size() - mTabBar->count());
  }

  lucent::info("rmlui",
               "menu built: {} tabs, {} panes, {} rows, {} of 4 readouts{}",
               tab_count(),
               pane_count(),
               row_count(),
               readout_count(),
               mUnknownRows ? ", with the unrecognised rows reported above" : "");

  if (mTabBar) {
    mTabBar->select(0); // stamps :selected on tab 0 and shows pane 0
  }
  mDoc->Hide(); // ESC shows it
}

MenuDocument::~MenuDocument() = default;

const psx::game::TitleIntSetting *MenuDocument::find_title_setting(std::string_view id) const {
  if (!mGame) {
    return nullptr;
  }
  for (const psx::game::TitleIntSetting &setting : mGame->runtime.titleIntSettings()) {
    if (id == setting.id) {
      return &setting;
    }
  }
  return nullptr;
}

void MenuDocument::add_title_setting_rows() {
  Rml::Element *host = mDoc->GetElementById(kTitleSettingsHostId);
  if (!host || !mGame) {
    return;
  }
  const std::span<const psx::game::TitleIntSetting> settings = mGame->runtime.titleIntSettings();
  if (settings.empty()) {
    return;
  }
  std::string rml = "<div class=\"section-heading spaced\">Enhancements</div>";
  for (const psx::game::TitleIntSetting &setting : settings) {
    rml += std::string("<select-button adjust=\"") + std::string(kTitleSettingPrefix) + setting.id + "\"><key>" +
           psx::ui::rmlTextMarkup(setting.label) + "</key><value></value></select-button>";
  }
  set_markup(host, rml);
}

void MenuDocument::dump() const {
  // Counts first, so the rows below arrive with their denominator.
  lucent::info("rmlui",
               "MENU: {} tabs, {} panes, {} rows, {} of 4 readouts, {} unrecognised row(s)",
               tab_count(),
               pane_count(),
               row_count(),
               readout_count(),
               mUnknownRows);
  for (int i = 0; i < (int)mPanes.size(); i++) {
    lucent::info("rmlui",
                 "  pane {}{}{}: {} row(s)",
                 i,
                 mTabBar && mTabBar->active() == i ? " [SELECTED]" : "",
                 mPanes[i]->shown() ? " [shown]" : "",
                 mPanes[i]->row_count());
    for (const MenuRow *r : mPanes[i]->rows()) {
      lucent::info("rmlui", "    {}", r->describe());
    }
  }
}

int MenuDocument::row_count() const {
  int n = 0;
  for (const MenuPane *p : mPanes) {
    n += p->row_count();
  }
  return n;
}

std::unique_ptr<RowBinding> MenuDocument::bind_row(Rml::Element *row) {
  if (!row) {
    return nullptr;
  }

  // An `action` row is a button even if it also carries `toggle` or `adjust`.
  std::string id = psx::ui::rmlAttributeText(*row, kAttrAction, "");
  if (!id.empty()) {
    return make_action_binding(
        [this, id] {
          run_action(id);
        },
        [this, id] {
          return id != "return_to_selector" || (mGame && mGame->session.returnAvailable());
        });
  }

  id = psx::ui::rmlAttributeText(*row, kAttrToggle, "");
  if (!id.empty()) {
    if (!ModRowModel::knows(id)) {
      mUnknownRows++;
      lucent::error("rmlui",
                    "menu row toggle=\"{}\" is unrecognised and inert ({} toggles exist)",
                    id,
                    ModRowModel::toggle_count());
      return nullptr;
    }
    return make_mod_toggle_binding(mGame ? &mGame->mods : nullptr, id);
  }

  id = psx::ui::rmlAttributeText(*row, kAttrAdjust, "");
  if (!id.empty()) {
    if (id == kWarpAreaId) {
      return make_warp_area_binding(&mWarp);
    }
    for (const char *devId : kDevCommandIds) {
      if (id == devId) {
        return make_dev_command_binding(&mDevCommands, id);
      }
    }
    if (id.rfind(kTitleSettingPrefix, 0) == 0) {
      const psx::game::TitleIntSetting *setting =
          find_title_setting(std::string_view(id).substr(kTitleSettingPrefix.size()));
      if (setting) {
        return make_title_setting_binding(setting, &mGame->mods);
      }
    }
    mUnknownRows++;
    lucent::error("rmlui", "menu row adjust=\"{}\" is unrecognised and inert", id);
    return nullptr;
  }

  mUnknownRows++;
  lucent::error("rmlui", "menu row has no action/toggle/adjust attribute; it is focusable but inert");
  return nullptr;
}

void MenuDocument::on_row_clicked(MenuRow &row) {
  // As the keyboard does: focus, then activate.
  row.focus();
  row.step(+1);
}

void MenuDocument::run_action(const std::string &id) {
  if (id == "quit") {
    lucent::info("rmlui", "quit from menu");
    exit(0);
  }
  if (id == "close") {
    hide();
    return;
  }
  if (id == "return_to_selector") {
    hide();
    if (mGame) {
      mGame->session.requestReturn();
    }
    return;
  }
  if (id == "warp_go") {
    if (mReadouts) {
      mReadouts->set_warp_status(mWarp.arm());
    }
    return;
  }
  if (id == "items_all" || id == "flag_read" || id == "flag_write") {
    if (mReadouts) {
      mReadouts->set_warp_status(id == "items_all"   ? mDevCommands.grantAllItems()
                                 : id == "flag_read" ? mDevCommands.readFlag()
                                                     : mDevCommands.writeFlag());
    }
    return;
  }
  // Sound Test: action="music_<n>" plays catalogued track n; action="music_stop" stops.
  if (id.rfind("music_", 0) == 0) {
    psx::game::TitleAudio *audio = mGame ? mGame->runtime.audio(mGame->core) : nullptr;
    if (!audio) {
      return;
    }
    audio->soundTestPlay(id == "music_stop" ? -1 : psx::config::parseInt(std::string_view(id).substr(6)).value_or(0));
    if (mReadouts) {
      mReadouts->update();
    }
    return;
  }
  lucent::error("rmlui", "menu row action=\"{}\" is unrecognised; the button does nothing", id);
}

void MenuDocument::show() {
  if (!mDoc) {
    return;
  }
  mVisible = true;
  mDoc->Show();
  if (mCtx) {
    mCtx->Update();
  }
  // Shows the tab's pane, refreshes every value and focuses the first row.
  if (mTabBar) {
    mTabBar->select(mTabBar->active());
  }
}

void MenuDocument::hide() {
  if (!mDoc) {
    return;
  }
  mVisible = false;
  if (mCtx) {
    if (Rml::Element *fe = mCtx->GetFocusElement()) {
      fe->Blur();
    }
  }
  mDoc->Hide();
}

void MenuDocument::update() {
  if (!mVisible) {
    return;
  }
  Component::update(); // hidden panes return early
}

// The tab bar's callback; a tab change goes through `mTabBar->select()`.
void MenuDocument::on_tab_selected(int index) {
  if (!mDoc) {
    return;
  }
  for (int i = 0; i < (int)mPanes.size(); i++) {
    mPanes[i]->set_shown(i == index);
  }
  Component::update(); // refresh the newly shown pane's rows and the readouts before focusing
  if (mCtx) {
    mCtx->Update();
  }
  if (index >= 0 && index < (int)mPanes.size()) {
    mPanes[index]->focus_first_row();
  }
}

MenuRow *MenuDocument::focused_row() const {
  if (!mCtx) {
    return nullptr;
  }
  Rml::Element *fe = mCtx->GetFocusElement();
  if (!fe) {
    return nullptr;
  }
  for (MenuPane *p : mPanes) {
    if (MenuRow *r = p->row_containing(fe)) {
      return r;
    }
  }
  return nullptr;
}

void MenuDocument::focus_step(int dir) {
  if (!mCtx) {
    return;
  }
  // RmlUi owns focus order (`tab-index: auto`), so Down/Up are its TAB / Shift+TAB.
  const int mods = dir < 0 ? Rml::Input::KM_SHIFT : 0;
  mCtx->ProcessKeyDown(Rml::Input::KI_TAB, mods);
  mCtx->ProcessKeyUp(Rml::Input::KI_TAB, mods);
  if (Rml::Element *f = mCtx->GetFocusElement()) {
    f->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
  }
}

void MenuDocument::activate_focused(int dir) {
  if (MenuRow *r = focused_row()) {
    r->step(dir);
  }
}

bool MenuDocument::handle_key(SDL_Keycode sdl_keycode) {
  if (!mVisible) {
    return false;
  }
  switch (sdl_keycode) {
  case SDLK_DOWN:
    focus_step(+1);
    return true;
  case SDLK_UP:
    focus_step(-1);
    return true;
  case SDLK_RIGHT:
  case SDLK_LEFT: {
    const int dir = sdl_keycode == SDLK_RIGHT ? +1 : -1;
    MenuRow *r = focused_row();
    // A row that steps with arrows takes them; otherwise they change tab.
    if (r && r->steps_with_arrows()) {
      r->step(dir);
    } else if (mTabBar) {
      mTabBar->select(mTabBar->active() + dir);
    }
    return true;
  }
  case SDLK_RETURN:
  case SDLK_KP_ENTER:
  case SDLK_SPACE:
    activate_focused(+1);
    return true;
  default:
    return false;
  }
}

} // namespace psx::ui
