// typed_config.h — the immutable launch configuration, built once by ProductHost and reached through Game.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace psx::config {

class Config {
public:
  // Read through the CVar registry's environment surface, so a value means here what it means to a BoolVar.
  static Config fromEnvironment();
  // Every variable this object reads, so the environment audit counts them as declared.
  static std::span<const std::string_view> environmentNames();

  // One log line naming every field.
  std::string describe() const;

  bool unpaced = false;                             // PSXPORT_NOPACE: never sleep to hold the present rate
  bool repl = false;                                // PSXPORT_REPL: the stdin REPL drives the product loop
  std::optional<bool> fps60;                        // PSXPORT_FPS60: this run's 60 fps choice over the saved setting
  std::optional<std::filesystem::path> packageRoot; // APPDIR: the mounted AppImage this process runs from
};

} // namespace psx::config
