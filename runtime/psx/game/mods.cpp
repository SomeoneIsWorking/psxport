#include "mods.h"

#include "config.h"

#include <lucent/log.h>

#include <algorithm>
#include <string>

namespace {

using psx::config::Layer;
using psx::config::Var;

Var<long> cv_aspect("aspect", ASPECT_4_3, {lucent::cvar::Persistence::Saved, "PSXPORT_ASPECT"});
Var<long> cv_ires("ires", 1, {lucent::cvar::Persistence::Saved, "PSXPORT_IRES"});
// PSXPORT_FPS60 is this run's choice at the Override layer; the saved one stays in the file.
Var<bool> cv_fps60("fps60", false, {lucent::cvar::Persistence::Saved, "PSXPORT_FPS60"});
Var<bool> cv_depth("depth", true, {lucent::cvar::Persistence::Saved, "PSXPORT_DEPTH"});
Var<double> cv_picker_selected_share("picker_selected_share",
                                     0.6,
                                     {lucent::cvar::Persistence::Saved, "PSXPORT_PICKER_SELECTED_SHARE"});
Var<double> cv_picker_slant("picker_slant", 0.1, {lucent::cvar::Persistence::Saved, "PSXPORT_PICKER_SLANT"});
Var<double> cv_picker_width_response("picker_width_response",
                                     0.25,
                                     {lucent::cvar::Persistence::Saved, "PSXPORT_PICKER_WIDTH_RESPONSE"});
const psx::config::Registration kRegistration{
    cv_aspect, cv_ires, cv_fps60, cv_depth, cv_picker_selected_share, cv_picker_slant, cv_picker_width_response};

constexpr long kMaxIres = 4;

} // namespace

void Mods::init(bool interpolates) {
  if (inited_) {
    return;
  }
  inited_ = true;
  temporalInterpolationSupported_ = interpolates;
  psx::config::load_settings();
  for (const psx::game::TitleIntSetting &setting : titleSettings_) {
    if (setting.var->has(Layer::Value)) {
      setting.var->set(setting.range.clamp(*setting.var->at(Layer::Value)), Layer::Value);
    }
  }
  reload();
  const bool requested = cv_fps60.get();
  const char *source = cv_fps60.has(Layer::Override) ? "PSXPORT_FPS60" : "settings";
  if (!temporalInterpolationSupported_ && requested) {
    lucent::warn(
        "fps60", "interpolated 60fps refused: this title declares no temporal interpolation (source: {})", source);
  }
  if (fps60 != 0) {
    lucent::info("fps60", "per-object interpolated 60fps on (source: {})", source);
  }
}

void Mods::reload() {
  aspect =
      static_cast<int>(cv_aspect.get() >= ASPECT_4_3 && cv_aspect.get() <= ASPECT_AUTO ? cv_aspect.get() : ASPECT_4_3);
  ires = static_cast<int>(std::clamp(cv_ires.get(), 0L, kMaxIres));
  pickerSelectedShare = static_cast<float>(cv_picker_selected_share.get());
  pickerSlantPerWidth = static_cast<float>(cv_picker_slant.get());
  pickerWidthResponse = static_cast<float>(cv_picker_width_response.get());
  fps60 = temporalInterpolationSupported_ && cv_fps60.get() ? 1 : 0;
  depth = cv_depth.get() ? 1 : 0;
}

// Only what the player changed reaches the file; an environment override stays this run's.
void Mods::save() const {
  const auto keep = [](auto &var, auto value) {
    if (value != static_cast<decltype(value)>(var.get())) {
      var.set(value, Layer::Value);
    }
  };
  keep(cv_aspect, static_cast<long>(aspect));
  keep(cv_ires, static_cast<long>(ires));
  keep(cv_depth, depth != 0);
  keep(cv_picker_selected_share, pickerSelectedShare);
  keep(cv_picker_slant, pickerSlantPerWidth);
  keep(cv_picker_width_response, pickerWidthResponse);
  if (temporalInterpolationSupported_) {
    keep(cv_fps60, fps60 != 0);
  }
  psx::config::save_settings();
}
