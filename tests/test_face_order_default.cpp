// A title may seed the player's face-order factory default, but the player's saved choice must
// remain the higher-precedence answer. The generic profile remains per-pixel depth.
#include "testutil.h"

#include "config_var.h"
#include "config_vars.h"
#include "mods.h"
#include "render_capabilities.h"

#include <filesystem>
#include <fstream>

namespace {

constexpr const char *kSettingsPath = "scratch_test_face_order_default.ini";

void test_generic_factory_default_is_depth() {
  const RenderCapabilities capabilities = RenderCapabilities::interpolatedNative();
  CHECK_EQ(capabilities.defaultFaceOrder, FACE_ORDER_DEPTH);

  Mods mods;
  mods.init(capabilities);
  CHECK_EQ(mods.face_order, FACE_ORDER_DEPTH);
}

void test_title_factory_can_select_authored_order() {
  const RenderCapabilities capabilities = RenderCapabilities::interpolatedNative(FACE_ORDER_AUTHORED);
  CHECK_EQ(capabilities.defaultFaceOrder, FACE_ORDER_AUTHORED);

  Mods mods;
  mods.init(capabilities);
  CHECK_EQ(mods.face_order, FACE_ORDER_AUTHORED);
}

void test_persisted_player_choice_wins_over_title_factory() {
  {
    std::ofstream settings(kSettingsPath);
    settings << "face_order=" << FACE_ORDER_DEPTH << '\n';
  }

  Mods mods;
  mods.init(RenderCapabilities::interpolatedNative(FACE_ORDER_AUTHORED));
  CHECK_EQ(mods.face_order, FACE_ORDER_DEPTH);
}

} // namespace

int main() {
  psx::config::cv_settings_path.set_text(psx::config::Layer::Override, kSettingsPath);
  std::error_code error;
  std::filesystem::remove(kSettingsPath, error);

  RUN(generic_factory_default_is_depth);
  RUN(title_factory_can_select_authored_order);
  RUN(persisted_player_choice_wins_over_title_factory);

  std::filesystem::remove(kSettingsPath, error);
  return pt_summary();
}
