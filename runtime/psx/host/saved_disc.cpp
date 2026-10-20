#include "saved_disc.h"

#include "disc.h"
#include "title_catalog.h"

#include <lucent/log.h>
#include <lucent/platform.h>
#include <lucent/text.h>

#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

namespace psx::host {

SavedDisc::SavedDisc(const TitleIdentity &title)
    : title_(title), file_([&title]() -> std::optional<std::filesystem::path> {
        const std::optional<std::filesystem::path> directory = lucent::platform::user_data_directory(title.slug);
        if (!directory) {
          return std::nullopt;
        }
        return *directory / "disc-path";
      }()) {}

std::optional<std::filesystem::path> SavedDisc::load() const {
  if (!file_) {
    return std::nullopt;
  }
  std::ifstream in(*file_);
  std::string line;
  if (!std::getline(in, line)) {
    return std::nullopt;
  }
  // A hand-edited file may carry a carriage return or spaces around the path.
  const std::string_view path = lucent::text::trim(line);
  if (path.empty()) {
    return std::nullopt;
  }
  return std::filesystem::path(path);
}

bool SavedDisc::save(const std::filesystem::path &image) const {
  if (!file_) {
    lucent::error("disc", "{} has no user-data directory to remember its disc in", title_.displayName);
    return false;
  }
  std::string error;
  if (!lucent::platform::ensure_user_data_directory(file_->parent_path(), error)) {
    lucent::error("disc", "cannot create {}: {}", file_->parent_path().string(), error);
    return false;
  }
  const std::filesystem::path pending = std::filesystem::path(*file_) += ".pending";
  {
    std::ofstream out(pending, std::ios::trunc);
    out << image.string() << '\n';
    if (!out.flush()) {
      lucent::error("disc", "cannot write {}", pending.string());
      return false;
    }
  }
  std::error_code renamed;
  std::filesystem::rename(pending, *file_, renamed);
  if (renamed) {
    lucent::error("disc", "cannot replace {}: {}", file_->string(), renamed.message());
    return false;
  }
  lucent::info("disc", "{} will read its disc from {}", title_.displayName, image.string());
  return true;
}

void SavedDisc::bind(psx::cd::Disc &disc) const {
  disc.env_key = title_.discEnvVar;
  disc.saved_path[0] = '\0';
  const std::optional<std::filesystem::path> image = load();
  if (!image) {
    return;
  }
  const std::string text = image->string();
  if (text.size() >= sizeof disc.saved_path) {
    lucent::warn("disc", "the saved disc path {} is too long to use", text);
    return;
  }
  std::memcpy(disc.saved_path, text.c_str(), text.size() + 1);
}

} // namespace psx::host
