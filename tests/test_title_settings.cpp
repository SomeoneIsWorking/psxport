// A title's integer setting: its stepped range, its menu row, its persistence and its comparison-run gate.
//
// The range is one rule shared by the row (stepping), the settings loader (a hand-edited file) and the
// title (reading the value back), so each is checked against that rule rather than a copy of it.

#include "config.h"
#include "config_vars.h"
#include "diagnostic_run.h"
#include "menu_row.h"
#include "mods.h"
#include "render_capabilities.h"
#include "testutil.h"
#include "title_settings.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

constexpr psx::config::SteppedRange kRange{0, 1000, 25};

// A settings directory outside any checkout, because Mods::save refuses to write inside one.
struct SettingsDir {
  fs::path root;
  fs::path file;

  explicit SettingsDir(const char *name) {
    root = fs::temp_directory_path() / (std::string("psxport-title-settings-") + name);
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    file = root / "psxport_settings.ini";
    psx::config::cv_settings_path.set_text(psx::config::Layer::Override, file.string());
  }

  ~SettingsDir() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }

  std::string body() const {
    std::ifstream in(file);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
};

} // namespace

static void test_range_snaps_to_the_step_grid_and_holds_the_ends() {
  CHECK_EQ(kRange.clamp(0), 0L);
  CHECK_EQ(kRange.clamp(1000), 1000L);
  CHECK_EQ(kRange.clamp(-50), 0L);
  CHECK_EQ(kRange.clamp(5000), 1000L);
  CHECK_EQ(kRange.clamp(12), 0L);
  CHECK_EQ(kRange.clamp(13), 25L);
  CHECK_EQ(kRange.clamp(300), 300L);
  CHECK_EQ(kRange.clamp(990), 1000L);
}

static void test_range_with_a_ragged_top_never_exceeds_max() {
  constexpr psx::config::SteppedRange ragged{0, 100, 30};
  CHECK_EQ(ragged.clamp(100), 90L);
  CHECK_EQ(ragged.clamp(95), 90L);
  CHECK_EQ(ragged.stepped(90, 1), 90L);
}

static void test_stepping_moves_one_step_and_saturates() {
  CHECK_EQ(kRange.stepped(0, 1), 25L);
  CHECK_EQ(kRange.stepped(25, -1), 0L);
  CHECK_EQ(kRange.stepped(0, -1), 0L);
  CHECK_EQ(kRange.stepped(1000, 1), 1000L);
  CHECK_EQ(kRange.stepped(975, 1), 1000L);
  CHECK_EQ(kRange.stepped(13, 1), 50L);
}

static void test_a_degenerate_range_holds_its_minimum() {
  constexpr psx::config::SteppedRange noStep{5, 50, 0};
  constexpr psx::config::SteppedRange empty{5, 5, 1};
  CHECK_EQ(noStep.clamp(30), 5L);
  CHECK_EQ(empty.stepped(5, 1), 5L);
}

static void test_the_row_shows_value_and_unit_and_steps_within_the_range() {
  SettingsDir dir("row");
  psx::config::IntVar var("PSXPORT_TEST_ROW_SETTING", 100, "test-only setting");
  const TitleIntSetting setting{"test_row", "Test", "%", &var, kRange};
  Mods mods;
  mods.declareTitleSettings(std::span<const TitleIntSetting>(&setting, 1));
  mods.init(RenderCapabilities::direct());
  auto binding = psx::ui::make_title_setting_binding(&setting, &mods);

  std::string text;
  CHECK(binding->text(text));
  CHECK_STREQ(text.c_str(), "100%");
  CHECK(binding->steps_with_arrows());
  binding->step(1);
  CHECK_EQ(var.get(), 125L);
  binding->step(-1);
  binding->step(-1);
  CHECK_EQ(var.get(), 75L);
  for (int i = 0; i < 100; ++i) {
    binding->step(-1);
  }
  CHECK_EQ(var.get(), 0L);
  CHECK(binding->text(text));
  CHECK_STREQ(text.c_str(), "0%");
  for (int i = 0; i < 100; ++i) {
    binding->step(1);
  }
  CHECK_EQ(var.get(), 1000L);
  CHECK(dir.body().find("PSXPORT_TEST_ROW_SETTING=1000") != std::string::npos);
}

static void test_the_chosen_value_is_persisted_and_reloaded_onto_the_grid() {
  SettingsDir dir("persist");
  {
    psx::config::IntVar var("PSXPORT_TEST_PERSISTED", 100, "test-only setting");
    const TitleIntSetting setting{"persisted", "Persisted", "%", &var, kRange};
    Mods mods;
    mods.declareTitleSettings(std::span<const TitleIntSetting>(&setting, 1));
    mods.init(RenderCapabilities::direct());
    var.set(psx::config::Layer::Value, 300);
    mods.save();
    CHECK(dir.body().find("PSXPORT_TEST_PERSISTED=300") != std::string::npos);
  }
  {
    std::ofstream out(dir.file, std::ios::app);
    out << "PSXPORT_TEST_PERSISTED=333\n";
  }
  psx::config::IntVar reloaded("PSXPORT_TEST_PERSISTED", 100, "test-only setting");
  const TitleIntSetting setting{"persisted", "Persisted", "%", &reloaded, kRange};
  Mods mods;
  mods.declareTitleSettings(std::span<const TitleIntSetting>(&setting, 1));
  mods.init(RenderCapabilities::direct());
  CHECK_EQ(reloaded.get(), 325L);
}

static void test_a_comparison_run_reads_the_neutral_value() {
  psx::config::IntVar var("PSXPORT_TEST_GATED", 300, "test-only setting");
  {
    psx::config::ScopedDiagnosticRun run(psx::config::DiagnosticRunMode::Product);
    CHECK_EQ(psx::config::enh_int(var), 300L);
  }
  psx::config::reset_for_test();
  {
    psx::config::ScopedDiagnosticRun run(psx::config::DiagnosticRunMode::CompareCandidate);
    CHECK_EQ(psx::config::enh_int(var), 0L);
  }
  psx::config::reset_for_test();
  {
    psx::config::ScopedDiagnosticRun run(psx::config::DiagnosticRunMode::CompareReference);
    CHECK_EQ(psx::config::enh_int(var), 0L);
  }
  psx::config::IntVar off("PSXPORT_TEST_GATED_OFF", 0, "test-only setting");
  CHECK_EQ(psx::config::enh_int(off), 0L);
}

int main() {
  RUN(range_snaps_to_the_step_grid_and_holds_the_ends);
  RUN(range_with_a_ragged_top_never_exceeds_max);
  RUN(stepping_moves_one_step_and_saturates);
  RUN(a_degenerate_range_holds_its_minimum);
  RUN(the_row_shows_value_and_unit_and_steps_within_the_range);
  RUN(the_chosen_value_is_persisted_and_reloaded_onto_the_grid);
  RUN(a_comparison_run_reads_the_neutral_value);
  return pt_summary();
}
