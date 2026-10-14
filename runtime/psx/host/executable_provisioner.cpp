#include "executable_provisioner.h"

#include "disc.h"
#include "fs_util.h"
#include "title_catalog.h"
#include "title_selection.h"

#include <lucent/log.h>

#include <string>
#include <system_error>
#include <vector>

namespace psx::host {
namespace {

// The title's disc, opened for one extraction and closed with it.
class TitleDisc {
public:
  explicit TitleDisc(const TitleIdentity &title) {
    disc_state_init(&disc_);
    disc_.env_key = title.discEnvVar;
  }
  ~TitleDisc() {
    psx::cd::closeDisc(disc_);
  }
  TitleDisc(const TitleDisc &) = delete;
  TitleDisc &operator=(const TitleDisc &) = delete;

  DiscState &state() {
    return disc_;
  }

private:
  DiscState disc_{};
};

} // namespace

Provisioning provisionExecutable(const TitleIdentity &title, const std::filesystem::path &target) {
  std::error_code error;
  if (std::filesystem::exists(target, error)) {
    return Provisioning::Present;
  }
  if (title.discExecutablePath.empty()) {
    lucent::warn(
        "provision", "{} is missing and {} names no executable on its disc", target.string(), title.displayName);
    return Provisioning::Unavailable;
  }
  const SelectionResult selection = [&title] {
    TitleDisc disc(title);
    return selectDiscExecutable(disc.state(), title);
  }();
  if (!selection) {
    lucent::warn("provision",
                 "{} is missing and {}'s disc did not supply it: {}",
                 target.string(),
                 title.displayName,
                 selection.detail);
    return Provisioning::Unavailable;
  }
  const std::string discPath(title.discExecutablePath);
  const std::vector<std::uint8_t> &bytes = selection.bytes;
  if (!Fs::writeFile(target.string(), bytes.data(), bytes.size())) {
    lucent::error("provision", "cannot write {}", target.string());
    return Provisioning::Unavailable;
  }
  lucent::info("provision", "extracted {} from {}'s disc to {}", discPath, title.displayName, target.string());
  return Provisioning::Extracted;
}

} // namespace psx::host
