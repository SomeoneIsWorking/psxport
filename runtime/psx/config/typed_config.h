// The immutable launch configuration, built once by ProductHost from the knob registry and reached through Game.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace psx::config {

// The installed package a process runs from.
struct Package {
  std::filesystem::path root;        // an AppImage's APPDIR, a macOS .app, or a Windows executable's directory
  std::filesystem::path assetParent; // the directory holding assets/
};

class Config {
public:
  static Config fromEnvironment();

  std::string describe() const;

  bool unpaced = false; // PSXPORT_NOPACE: never sleep to hold the present rate
  bool repl = false;    // PSXPORT_REPL: the stdin REPL drives the product loop
  std::optional<Package> package;
};

} // namespace psx::config
