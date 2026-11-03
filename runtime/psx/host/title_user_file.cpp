#include "title_user_file.h"

#include "title_catalog.h"

#include <lucent/log.h>
#include <lucent/platform.h>

#include <fstream>
#include <system_error>

namespace psx::host {

TitleUserFile::TitleUserFile(const TitleIdentity &title, std::string_view name)
    : file_([&title, name]() -> std::optional<std::filesystem::path> {
        const std::optional<std::filesystem::path> directory = lucent::platform::user_data_directory(title.slug);
        if (!directory) {
          return std::nullopt;
        }
        return *directory / name;
      }()) {}

std::optional<std::string> TitleUserFile::replace(std::ios::openmode mode,
                                                  const std::function<void(std::ostream &)> &write) const {
  if (!file_) {
    return std::string("the title has no user-data directory");
  }
  std::string error;
  if (!lucent::platform::ensure_user_data_directory(file_->parent_path(), error)) {
    return lucent::format("cannot create {}: {}", file_->parent_path().string(), error);
  }
  const std::filesystem::path pending = std::filesystem::path(*file_) += ".pending";
  {
    std::ofstream out(pending, mode | std::ios::out | std::ios::trunc);
    write(out);
    if (!out.flush()) {
      return lucent::format("cannot write {}", pending.string());
    }
  }
  std::error_code renamed;
  std::filesystem::rename(pending, *file_, renamed);
  if (renamed) {
    return lucent::format("cannot replace {}: {}", file_->string(), renamed.message());
  }
  return std::nullopt;
}

} // namespace psx::host
