#include "typed_config.h"

#include "config_var.h"

#include <array>

namespace psx::config {
namespace {

inline constexpr std::string_view kNoPace = "PSXPORT_NOPACE";
inline constexpr std::string_view kRepl = "PSXPORT_REPL";
inline constexpr std::string_view kFps60 = "PSXPORT_FPS60";
inline constexpr std::array<std::string_view, 3> kNames{kNoPace, kRepl, kFps60};
// Not a PSXPORT_ knob, so the audit never sees it.
inline constexpr std::string_view kAppDir = "APPDIR";

// Unset is no answer; set, it resolves as a BoolVar's environment layer does.
std::optional<bool> flag(std::string_view name) {
  const std::string key(name);
  if (!detail::env_present(key.c_str())) {
    return std::nullopt;
  }
  return detail::env_bool(key.c_str());
}

// The AppImage runtime exports its mount point; a relative value is not one.
std::optional<std::filesystem::path> appDir() {
  const std::string key(kAppDir);
  std::filesystem::path root(detail::env_text(key.c_str()));
  if (!root.is_absolute()) {
    return std::nullopt;
  }
  return root;
}

const char *onOff(bool value) {
  return value ? "on" : "off";
}

} // namespace

Config Config::fromEnvironment() {
  Config config;
  config.unpaced = flag(kNoPace).value_or(false);
  config.repl = flag(kRepl).value_or(false);
  config.fps60 = flag(kFps60);
  config.packageRoot = appDir();
  return config;
}

std::span<const std::string_view> Config::environmentNames() {
  return kNames;
}

std::string Config::describe() const {
  std::string text = "nopace=";
  text += onOff(unpaced);
  text += " repl=";
  text += onOff(repl);
  text += " fps60=";
  text += fps60.has_value() ? onOff(*fps60) : "saved";
  text += " package=";
  text += packageRoot.has_value() ? packageRoot->string() : "none";
  return text;
}

} // namespace psx::config
