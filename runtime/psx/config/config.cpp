#include "config.h"
#include "config_vars.h"
#include "number_text.h"
#include "typed_config.h"

#include <lucent/config.h>
#include <lucent/log.h>
#include <lucent/text.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <system_error>

namespace psx::config {

Registration::Registration(std::initializer_list<std::reference_wrapper<lucent::cvar::VarBase>> vars) {
  for (lucent::cvar::VarBase &var : vars) {
    lucent::cvar::register_var(var);
  }
}

namespace {

Var<bool> cv_noaudio("PSXPORT_NOAUDIO", false, kRunOnly);
Var<long> cv_debug_server("PSXPORT_DEBUG_SERVER", 0, kRunOnly);
Var<std::string> cv_store_observe("PSXPORT_STORE_OBSERVE", "", kRunOnly);
Var<std::string> cv_ramdump("PSXPORT_RAMDUMP", "", kRunOnly);
Var<std::string> cv_load_state("PSXPORT_LOAD_STATE", "", kRunOnly);
Var<std::string> cv_bug_report_dir("PSXPORT_BUG_REPORT_DIR", "", kRunOnly);
Var<std::string> cv_pad_record("PSXPORT_PAD_RECORD", "", kRunOnly);
Var<std::string> cv_pad_replay("PSXPORT_PAD_REPLAY", "", kRunOnly);
Var<std::string> cv_pad_resume("PSXPORT_PAD_RESUME", "", kRunOnly);
Var<long> cv_watchdog("PSXPORT_WATCHDOG", 3, kRunOnly);
Var<long> cv_spin_ticks("PSXPORT_SPIN_TICKS", 8000000, kRunOnly);
Var<long> cv_spin_runs("PSXPORT_SPIN_RUNS", 48, kRunOnly);
Var<long> cv_watchdog_boot("PSXPORT_WATCHDOG_BOOT", -1, kRunOnly);
Var<long> cv_native_frames("PSXPORT_NATIVE_FRAMES", 0, kRunOnly);
Var<std::string> cv_asset_dir("PSXPORT_ASSET_DIR", "", kRunOnly);
Var<bool> cv_show_window("PSXPORT_SHOW_WINDOW", false, kRunOnly);
Var<std::string> cv_settings_path("PSXPORT_SETTINGS", "psxport_settings.ini", kRunOnly);
// Arms the producer census, a diagnostic the player's product never pays for (ot_attr.h).
Var<bool> cv_producers("PSXPORT_PRODUCERS", false, kRunOnly);
Var<std::string> cv_render_path("PSXPORT_RENDER_PATH", "record", kSaved);
Var<long> cv_lightrec_fallback_block_limit("PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT",
                                           static_cast<long>(psx::cpu::kDefaultMaxFallbackBlocksPerExecution),
                                           kRunOnly);
// A settings file must never arm a gate that rewrites the continued run's state.
Var<std::string> cv_override_diff("PSXPORT_OVERRIDE_DIFF", "", kRunOnly);
Var<long> cv_override_diff_first("PSXPORT_OVERRIDE_DIFF_FIRST",
                                 static_cast<long>(psx::cpu::kDefaultDifferentialFirstCalls),
                                 kRunOnly);
Var<long> cv_override_diff_every("PSXPORT_OVERRIDE_DIFF_EVERY",
                                 static_cast<long>(psx::cpu::kDefaultDifferentialEveryKth),
                                 kRunOnly);
Var<long> cv_override_diff_dead_stack("PSXPORT_OVERRIDE_DIFF_DEAD_STACK",
                                      static_cast<long>(psx::cpu::kDefaultDifferentialDeadStackBytes),
                                      kRunOnly);
Var<std::string> cv_override_diff_report("PSXPORT_OVERRIDE_DIFF_REPORT",
                                         std::string(psx::cpu::kDefaultDifferentialReportPath),
                                         kRunOnly);
Var<std::string> cv_reach_report("PSXPORT_REACH_REPORT", "", kRunOnly);
Var<std::string> cv_owners_dir("PSXPORT_OWNERS_DIR", "scratch/owners", kRunOnly);
Var<bool> cv_fullscreen("PSXPORT_FULLSCREEN", false, kRunOnly);
Var<bool> cv_windowed("PSXPORT_WINDOWED", true, kRunOnly);
Var<std::string> cv_picker_titles("PSXPORT_PICKER_TITLES", "", kRunOnly);
Var<bool> cv_nopace("PSXPORT_NOPACE", false, kRunOnly);
Var<bool> cv_repl("PSXPORT_REPL", false, kRunOnly);
// Lucent reads these two itself; declared so the audit and the report know them.
Var<std::string> cv_debug_channels("PSXPORT_DEBUG", "", kRunOnly);
Var<std::string> cv_log_file("PSXPORT_LOG_FILE", "", kRunOnly);

const Registration kRegistration{
    cv_noaudio,
    cv_debug_server,
    cv_store_observe,
    cv_ramdump,
    cv_load_state,
    cv_bug_report_dir,
    cv_pad_record,
    cv_pad_replay,
    cv_pad_resume,
    cv_watchdog,
    cv_spin_ticks,
    cv_spin_runs,
    cv_watchdog_boot,
    cv_native_frames,
    cv_asset_dir,
    cv_show_window,
    cv_settings_path,
    cv_producers,
    cv_render_path,
    cv_lightrec_fallback_block_limit,
    cv_override_diff,
    cv_override_diff_first,
    cv_override_diff_every,
    cv_override_diff_dead_stack,
    cv_override_diff_report,
    cv_reach_report,
    cv_owners_dir,
    cv_fullscreen,
    cv_windowed,
    cv_picker_titles,
    cv_nopace,
    cv_repl,
    cv_debug_channels,
    cv_log_file,
};

bool isExternal(std::string_view name) {
  return name == cv_debug_channels.name() || name == cv_log_file.name();
}

// The keys titles passed to launch_setting(), known to the audit once read.
struct LaunchKeys {
  std::mutex mutex;
  std::set<std::string> keys;
};
constinit LaunchKeys *launchKeys = nullptr;

LaunchKeys &launchKeyState() {
  if (launchKeys == nullptr) {
    launchKeys = new LaunchKeys;
  }
  return *launchKeys;
}

std::string dotenvValue(std::string_view key) {
  std::ifstream in(".env");
  std::string line;
  while (std::getline(in, line)) {
    const std::string_view entry = lucent::text::trim(line);
    if (entry.size() <= key.size() || entry.substr(0, key.size()) != key || entry[key.size()] != '=') {
      continue;
    }
    return std::string(lucent::text::trim(entry.substr(key.size() + 1)));
  }
  return {};
}

// Saves never land in a source checkout, where they would overwrite a tracked file.
bool insideCheckout(const std::string &path) {
  std::error_code error;
  for (std::filesystem::path dir(path); !dir.empty(); dir = dir.parent_path()) {
    if (std::filesystem::exists(dir / ".git", error)) {
      return true;
    }
    if (dir == dir.parent_path()) {
      return false;
    }
  }
  return false;
}

bool reported = false;

void reportExitAudit() {
  for (const std::string &line : cvar_report_lines(false)) {
    lucent::info("cfg", "at exit: {}", line);
  }
}

} // namespace

const char *layer_name(Layer layer) {
  switch (layer) {
  case Layer::Default:
    return "default";
  case Layer::Value:
    return "settings";
  case Layer::Override:
    return "env";
  case Layer::Runtime:
    return "runtime";
  }
  return "?";
}

bool show_window() {
  return cv_show_window.get();
}

void applyPackagedDefaults(const std::filesystem::path &packageRoot) {
  cv_show_window.set(true, Layer::Default);
  cv_asset_dir.set((packageRoot / "usr" / "share" / "psxport").string(), Layer::Default);
}

psx::cpu::FallbackPolicy lightrec_fallback_policy() {
  return psx::cpu::fallbackPolicyFromConfigured(cv_lightrec_fallback_block_limit.get());
}

psx::cpu::OverrideDifferentialConfig override_differential_config() {
  return psx::cpu::overrideDifferentialConfigFrom(cv_override_diff.get(),
                                                  cv_override_diff_first.get(),
                                                  cv_override_diff_every.get(),
                                                  cv_override_diff_dead_stack.get(),
                                                  cv_override_diff_report.get());
}

std::string function_reach_report_path() {
  return cv_reach_report.get();
}

std::string owners_dir() {
  return cv_owners_dir.get();
}

bool no_audio() {
  return cv_noaudio.get();
}

bool producer_census() {
  return cv_producers.get();
}

int debug_server_port() {
  constexpr long kDefaultPort = 5959;
  constexpr long kMaxPort = 65535;
  const long port = cv_debug_server.get();
  if (port <= 0 || port > kMaxPort) {
    return 0;
  }
  return static_cast<int>(port == 1 ? kDefaultPort : port);
}

std::optional<std::vector<std::uint32_t>> store_observe_pcs() {
  return parseUnsignedList(cv_store_observe.get(), 16);
}

std::string ramdump_path() {
  return cv_ramdump.get();
}

std::string load_state_path() {
  return cv_load_state.get();
}

std::string bug_report_dir() {
  return cv_bug_report_dir.get();
}

std::string pad_record_path() {
  return cv_pad_record.get();
}

std::string pad_replay_path() {
  return cv_pad_replay.get();
}

std::string pad_resume_path() {
  return cv_pad_resume.get();
}

long watchdog_seconds() {
  return cv_watchdog.get();
}

long spin_ticks() {
  return cv_spin_ticks.get();
}

long spin_runs() {
  return cv_spin_runs.get();
}

long watchdog_boot_seconds() {
  return cv_watchdog_boot.get();
}

long native_frames() {
  return cv_native_frames.get();
}

std::string asset_dir() {
  return cv_asset_dir.get();
}

std::string settings_path() {
  return cv_settings_path.get();
}

bool fullscreen_requested() {
  return cv_fullscreen.get() || !cv_windowed.get();
}

std::string picker_titles() {
  return cv_picker_titles.get();
}

present::RenderPath render_path(present::RenderPath fallback) {
  const std::string &text = cv_render_path.get();
  present::RenderPath path = fallback;
  if (!present::parseRenderPath(text.c_str(), &path)) {
    lucent::warn("cfg",
                 "PSXPORT_RENDER_PATH='{}' matched no render path, using '{}'. Valid: record | device.",
                 text,
                 present::renderPathName(fallback));
    return fallback;
  }
  return path;
}

Config Config::fromEnvironment() {
  Config config;
  config.unpaced = cv_nopace.get();
  config.repl = cv_repl.get();
  std::filesystem::path root(lucent::config::text("APPDIR"));
  if (root.is_absolute()) {
    config.packageRoot = root;
  }
  return config;
}

std::string Config::describe() const {
  return lucent::format("nopace={} repl={} package={}",
                        unpaced ? "on" : "off",
                        repl ? "on" : "off",
                        packageRoot.has_value() ? packageRoot->string() : std::string("none"));
}

std::string launch_setting(std::initializer_list<const char *> keys) {
  std::vector<std::string_view> named;
  for (const char *key : keys) {
    if (key != nullptr && *key != '\0') {
      named.emplace_back(key);
    }
  }
  {
    LaunchKeys &state = launchKeyState();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (const std::string_view key : named) {
      state.keys.emplace(key);
    }
  }
  for (const std::string_view key : named) {
    std::string value(lucent::text::trim(lucent::config::text(key)));
    if (!value.empty()) {
      return value;
    }
  }
  for (const std::string_view key : named) {
    std::string value = dotenvValue(key);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

EnvAudit audit_environment() {
  std::set<std::string> declared;
  lucent::cvar::enumerate([&declared](lucent::cvar::VarBase &var) {
    declared.insert(var.env_name());
  });
  std::set<std::string> identity;
  {
    LaunchKeys &state = launchKeyState();
    std::lock_guard<std::mutex> lock(state.mutex);
    identity = state.keys;
  }
  EnvAudit audit;
  for (const std::string &entry : lucent::config::active()) {
    if (entry.rfind("PSXPORT_", 0) != 0) {
      continue;
    }
    audit.set_in_env.push_back(entry);
    const std::string name = entry.substr(0, entry.find('='));
    if (declared.contains(name)) {
      audit.declared.push_back(name);
    } else if (identity.contains(name)) {
      audit.identity.push_back(name);
    } else {
      audit.unknown.push_back(name);
    }
  }
  return audit;
}

std::vector<std::string> cvar_report_lines(bool withKnobs) {
  std::vector<std::string> lines;
  if (withKnobs) {
    lucent::cvar::enumerate([&lines](lucent::cvar::VarBase &var) {
      std::string line = lucent::format("{} = {} [{}]", var.name(), var.effective_string(), layer_name(var.layer()));
      for (const Layer layer : {Layer::Value, Layer::Override, Layer::Runtime}) {
        if (layer != var.layer() && var.has(layer)) {
          line += lucent::format(" ({}={})", layer_name(layer), var.layer_string(layer));
        }
      }
      lines.push_back(std::move(line));
    });
    std::sort(lines.begin(), lines.end());
  }
  const EnvAudit audit = audit_environment();
  lines.push_back(lucent::format("env audit: {} PSXPORT_* set -> {} declared, {} title-supplied, {} unknown",
                                 audit.set_in_env.size(),
                                 audit.declared.size(),
                                 audit.identity.size(),
                                 audit.unknown.size()));
  for (const std::string &name : audit.unknown) {
    lines.push_back(lucent::format("UNKNOWN {} is set and matched nothing", name));
  }
  return lines;
}

void report() {
  for (const std::string &line : cvar_report_lines(true)) {
    lucent::log(line.starts_with("UNKNOWN") ? lucent::Level::Warn : lucent::Level::Info, "cfg", line);
  }
}

void report_once() {
  if (reported) {
    return;
  }
  reported = true;
  report();
  std::atexit(reportExitAudit);
}

bool set_runtime(std::string_view name, std::string_view text) {
  lucent::cvar::VarBase *var = lucent::cvar::find(name);
  if (var == nullptr || isExternal(name)) {
    lucent::warn("cfg", "runtime: {} is not a knob this registry resolves; ignored", name);
    return false;
  }
  if (!var->apply(text, Layer::Runtime)) {
    lucent::warn("cfg", "runtime: \"{}\" is not a valid value for {}; ignored", text, name);
    return false;
  }
  lucent::info("cfg", "runtime: {} = {} (this run only)", name, var->effective_string());
  return true;
}

bool clear_runtime(std::string_view name) {
  lucent::cvar::VarBase *var = lucent::cvar::find(name);
  if (var == nullptr) {
    lucent::warn("cfg", "runtime: {} is not a knob; ignored", name);
    return false;
  }
  var->clear(Layer::Runtime);
  lucent::info("cfg", "runtime: {} cleared -> {} [{}]", name, var->effective_string(), layer_name(var->layer()));
  return true;
}

void note_runtime_debug_channels(std::string_view channels) {
  cv_debug_channels.set(std::string(channels), Layer::Runtime);
}

void load_settings() {
  lucent::cvar::load_file(cv_settings_path.get().c_str());
}

bool save_settings() {
  const std::string &path = cv_settings_path.get();
  if (insideCheckout(path)) {
    lucent::warn("cfg", "refusing to save settings into a source checkout: {}; point PSXPORT_SETTINGS elsewhere", path);
    return false;
  }
  if (!lucent::cvar::save_file(path.c_str())) {
    lucent::error("cfg", "could not write settings file {}", path);
    return false;
  }
  return true;
}

} // namespace psx::config
