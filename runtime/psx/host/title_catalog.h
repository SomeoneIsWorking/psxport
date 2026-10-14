#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

class GameRuntime;

namespace psx::host {

// One title's executable and disc. What a selection means: serial plus file identity, never the label.
struct TitleIdentity {
  std::string_view displayName;
  std::string_view serial;
  std::string_view slug; // directory under the provisioning root: <root>/<slug>/<serial>
  std::size_t fileSize = 0;
  std::string_view sha256;
  // PS-X EXE header cross-check.
  std::uint32_t entry = 0;
  std::uint32_t globalPointer = 0;
  std::uint32_t textAddress = 0;
  std::uint32_t textSize = 0;
  std::uint32_t stackAddress = 0;
  std::uint32_t stackOffset = 0;
  // The title's own disc-image key, read before PSXPORT_DISC; null leaves only the generic keys.
  const char *discEnvVar = nullptr;
  // The boot executable's ISO9660 path on the disc, "/"-separated: SCUS_942.28, TEKKEN3/SLUS_004.02.
  std::string_view discExecutablePath;

  // Where the provisioned executable lives.
  std::filesystem::path defaultExecutable(const std::filesystem::path &provisioningRoot) const {
    return provisioningRoot / std::string(slug) / std::string(serial);
  }
};

// What a multi-title product supplies. Index i is catalog entry i everywhere.
class TitleCatalog {
public:
  virtual ~TitleCatalog() = default;
  virtual std::string_view productName() const = 0; // the window title, picker heading and help name
  // Where `<slug>/<serial>` is provisioned; relative paths are from the working directory.
  virtual std::filesystem::path provisioningRoot() const = 0;
  virtual std::span<const TitleIdentity> titles() const = 0;
  // Installed before that title's Game exists.
  virtual GameRuntime &runtime(std::size_t index) const = 0;
};

} // namespace psx::host
