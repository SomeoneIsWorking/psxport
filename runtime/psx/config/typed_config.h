// The immutable launch configuration, built once by ProductHost from the knob registry and reached through Game.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace psx::config {

class Config {
public:
  static Config fromEnvironment();

  std::string describe() const;

  bool unpaced = false;                             // PSXPORT_NOPACE: never sleep to hold the present rate
  bool repl = false;                                // PSXPORT_REPL: the stdin REPL drives the product loop
  std::optional<std::filesystem::path> packageRoot; // APPDIR: the mounted AppImage this process runs from
};

} // namespace psx::config
