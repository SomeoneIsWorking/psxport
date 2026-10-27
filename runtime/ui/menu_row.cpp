#include "menu_row.h"

#include "mod_row_model.h"
#include "mods.h"
#include "rml_text.h"
#include "title_settings.h"

#include <RmlUi/Core/StringUtilities.h>

#include <utility>

namespace psx::ui {
namespace {

class ModToggleBinding final : public RowBinding {
public:
  ModToggleBinding(Mods *mods, std::string id) : mMods(mods), mId(std::move(id)) {}
  bool available() const override {
    return mMods && ModRowModel::available(*mMods, mId);
  }
  bool text(std::string &out) const override {
    return mMods && ModRowModel::value_text(*mMods, mId, out);
  }
  void step(int) override {
    if (mMods) {
      ModRowModel::toggle(*mMods, mId);
    }
  }

private:
  Mods *mMods;
  std::string mId;
};

class TitleSettingBinding final : public RowBinding {
public:
  TitleSettingBinding(const psx::game::TitleIntSetting *setting, Mods *mods) : mSetting(setting), mMods(mods) {}
  bool text(std::string &out) const override {
    out = std::to_string(mSetting->range.clamp(mSetting->var->get())) + mSetting->unit;
    return true;
  }
  void step(int dir) override {
    mSetting->var->set(mSetting->range.stepped(mSetting->var->get(), dir), psx::config::Layer::Value);
    if (mMods) {
      mMods->save();
    }
  }
  bool steps_with_arrows() const override {
    return true;
  }

private:
  const psx::game::TitleIntSetting *mSetting;
  Mods *mMods;
};

class ActionBinding final : public RowBinding {
public:
  ActionBinding(std::function<void()> action, std::function<bool()> available)
      : mAction(std::move(action)), mAvailable(std::move(available)) {}
  bool available() const override {
    return !mAvailable || mAvailable();
  }
  void step(int) override {
    if (mAction) {
      mAction();
    }
  }

private:
  std::function<void()> mAction;
  std::function<bool()> mAvailable;
};

} // namespace

std::unique_ptr<RowBinding> make_mod_toggle_binding(Mods *mods, std::string id) {
  return std::make_unique<ModToggleBinding>(mods, std::move(id));
}
std::unique_ptr<RowBinding> make_title_setting_binding(const psx::game::TitleIntSetting *setting, Mods *mods) {
  return std::make_unique<TitleSettingBinding>(setting, mods);
}
std::unique_ptr<RowBinding> make_action_binding(std::function<void()> action, std::function<bool()> available) {
  return std::make_unique<ActionBinding>(std::move(action), std::move(available));
}

MenuRow::MenuRow(Rml::Element *root, std::unique_ptr<RowBinding> binding, std::function<void(MenuRow &)> on_click)
    : Component(root), mBinding(std::move(binding)), mOnClick(std::move(on_click)) {
  mValue = mRoot ? mRoot->QuerySelector("value") : nullptr;
  listen(mRoot, Rml::EventId::Click, [this](Rml::Event &) {
    if (mOnClick) {
      mOnClick(*this);
    }
  });
}

void MenuRow::update() {
  std::string txt;
  if (mBinding && mBinding->text(txt)) {
    set_text(mValue, txt);
  }
  Component::update();
}

void MenuRow::step(int dir) {
  if (mBinding) {
    mBinding->step(dir);
  }
  update();
}

std::string MenuRow::describe() const {
  if (!mRoot) {
    return "<no element>";
  }
  std::string kind = "INERT", id;
  for (const char *attr : {"action", "toggle", "adjust"}) {
    const std::string v = psx::ui::rmlAttributeText(*mRoot, attr, "");
    if (!v.empty()) {
      kind = attr;
      id = v;
      break;
    }
  }
  Rml::Element *key = mRoot->QuerySelector("key");
  // Decoded, so an entity reads as the glyph drawn.
  const std::string label = key ? Rml::StringUtilities::DecodeRml(key->GetInnerRML()) : std::string();
  const std::string value = mValue ? Rml::StringUtilities::DecodeRml(mValue->GetInnerRML()) : std::string();
  return kind + "=\"" + id + "\" \"" + label + "\" = \"" + value + "\"" + (mBinding ? "" : "   <-- NO BINDING (inert)");
}

} // namespace psx::ui
