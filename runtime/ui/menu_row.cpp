#include "menu_row.h"

#include "dev_command_control.h"
#include "mods.h"
#include "render_path_control.h"
#include "title_settings.h"
#include "warp_control.h"

#include <RmlUi/Core/StringUtilities.h>

#include <utility>

namespace psx::ui {
namespace {

class ModToggleBinding final : public RowBinding {
public:
  ModToggleBinding(Mods *mods, std::string id) : mMods(mods), mId(std::move(id)) {}
  bool available() const override {
    return mMods && ModRowModel::available(*mMods, RowKind::Toggle, mId);
  }
  bool text(std::string &out) const override {
    return mMods && ModRowModel::value_text(*mMods, RowKind::Toggle, mId, out);
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

class ModAdjustBinding final : public RowBinding {
public:
  ModAdjustBinding(Mods *mods, std::string id) : mMods(mods), mId(std::move(id)) {}
  bool text(std::string &out) const override {
    return mMods && ModRowModel::value_text(*mMods, RowKind::Adjust, mId, out);
  }
  void step(int dir) override {
    if (mMods) {
      ModRowModel::adjust(*mMods, mId, dir);
    }
  }
  bool steps_with_arrows() const override {
    return true;
  }

private:
  Mods *mMods;
  std::string mId;
};

class TitleSettingBinding final : public RowBinding {
public:
  TitleSettingBinding(const TitleIntSetting *setting, Mods *mods) : mSetting(setting), mMods(mods) {}
  bool text(std::string &out) const override {
    out = std::to_string(mSetting->range.clamp(mSetting->var->get())) + mSetting->unit;
    return true;
  }
  void step(int dir) override {
    mSetting->var->set(psx::config::Layer::Value, mSetting->range.stepped(mSetting->var->get(), dir));
    if (mMods) {
      mMods->save();
    }
  }
  bool steps_with_arrows() const override {
    return true;
  }

private:
  const TitleIntSetting *mSetting;
  Mods *mMods;
};

class WarpAreaBinding final : public RowBinding {
public:
  explicit WarpAreaBinding(WarpControl *warp) : mWarp(warp) {}
  bool text(std::string &out) const override {
    if (!mWarp) {
      return false;
    }
    out = mWarp->currentLabel();
    return true;
  }
  void step(int dir) override {
    if (mWarp) {
      mWarp->adjust(dir);
    }
  }
  bool steps_with_arrows() const override {
    return true;
  }

private:
  WarpControl *mWarp;
};

class DevCommandBinding final : public RowBinding {
public:
  DevCommandBinding(DevCommandControl *commands, std::string id) : mCommands(commands), mId(std::move(id)) {}
  bool text(std::string &out) const override {
    if (!mCommands) {
      return false;
    }
    out = std::to_string(value());
    return true;
  }
  void step(int dir) override {
    if (!mCommands) {
      return;
    }
    if (mId == "warp_entry") {
      mCommands->adjustEntry(dir);
    } else if (mId == "flag_index") {
      mCommands->adjustFlagIndex(dir);
    } else {
      mCommands->adjustFlagValue(dir);
    }
  }
  bool steps_with_arrows() const override {
    return true;
  }

private:
  int value() const {
    if (mId == "warp_entry") {
      return mCommands->entry();
    }
    return mId == "flag_index" ? mCommands->flagIndex() : mCommands->flagValue();
  }

  DevCommandControl *mCommands;
  std::string mId;
};

class RenderPathBinding final : public RowBinding {
public:
  explicit RenderPathBinding(RenderPathControl *render_path) : mRenderPath(render_path) {}
  bool available() const override {
    return mRenderPath && mRenderPath->available();
  }
  bool text(std::string &out) const override {
    if (!mRenderPath) {
      return false;
    }
    out = mRenderPath->currentLabel();
    return true;
  }
  void step(int) override {
    if (mRenderPath) {
      mRenderPath->cycle();
    }
  }

private:
  RenderPathControl *mRenderPath;
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
std::unique_ptr<RowBinding> make_mod_adjust_binding(Mods *mods, std::string id) {
  return std::make_unique<ModAdjustBinding>(mods, std::move(id));
}
std::unique_ptr<RowBinding> make_title_setting_binding(const TitleIntSetting *setting, Mods *mods) {
  return std::make_unique<TitleSettingBinding>(setting, mods);
}
std::unique_ptr<RowBinding> make_warp_area_binding(WarpControl *warp) {
  return std::make_unique<WarpAreaBinding>(warp);
}
std::unique_ptr<RowBinding> make_dev_command_binding(DevCommandControl *commands, std::string id) {
  return std::make_unique<DevCommandBinding>(commands, std::move(id));
}
std::unique_ptr<RowBinding> make_render_path_binding(RenderPathControl *render_path) {
  return std::make_unique<RenderPathBinding>(render_path);
}
std::unique_ptr<RowBinding> make_action_binding(std::function<void()> action, std::function<bool()> available) {
  return std::make_unique<ActionBinding>(std::move(action), std::move(available));
}

MenuRow::MenuRow(Rml::Element *root, std::unique_ptr<RowBinding> binding, std::function<void(MenuRow &)> on_click)
    : Component(root), mBinding(std::move(binding)), mOnClick(std::move(on_click)) {
  mValue = mRoot ? mRoot->QuerySelector("value") : nullptr;
  // The listener's lifetime is this row's — see ui_event.h for what that replaces.
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
    const std::string v = mRoot->GetAttribute<Rml::String>(attr, "");
    if (!v.empty()) {
      kind = attr;
      id = v;
      break;
    }
  }
  Rml::Element *key = mRoot->QuerySelector("key");
  // DECODED, not the raw markup: the whole point of the text boundary is that these can differ,
  // and a dump that echoed the markup would have shown "&middot;" as correct.
  const std::string label = key ? Rml::StringUtilities::DecodeRml(key->GetInnerRML()) : std::string();
  const std::string value = mValue ? Rml::StringUtilities::DecodeRml(mValue->GetInnerRML()) : std::string();
  return kind + "=\"" + id + "\" \"" + label + "\" = \"" + value + "\"" + (mBinding ? "" : "   <-- NO BINDING (inert)");
}

} // namespace psx::ui
