#include "saved_disc.h"

#include "disc.h"
#include "title_catalog.h"

#include <lucent/log.h>
#include <lucent/text.h>

#include <cstring>
#include <fstream>
#include <string>

namespace psx::host {

SavedDisc::SavedDisc(const TitleIdentity &title) : title_(title), file_(title, "disc-path") {}

std::optional<std::filesystem::path> SavedDisc::load() const {
  if (!file_.path()) {
    return std::nullopt;
  }
  std::ifstream in(*file_.path());
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
  if (const std::optional<std::string> error = file_.replace(std::ios::out, [&image](std::ostream &out) {
        out << image.string() << '\n';
      })) {
    lucent::error("disc", "{} cannot remember its disc: {}", title_.displayName, *error);
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
