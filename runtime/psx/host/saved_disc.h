#pragma once

#include <filesystem>
#include <optional>

struct DiscState;

namespace psx::host {

struct TitleIdentity;

// The disc image the player chose at first run, kept in the title's user-data directory.
class SavedDisc {
public:
  explicit SavedDisc(const TitleIdentity &title);

  std::optional<std::filesystem::path> load() const;
  // Replaces the saved choice atomically; false when it cannot be written.
  bool save(const std::filesystem::path &image) const;
  // Points `disc` at the title's own key and then at the saved choice (disc.h resolution order).
  void bind(DiscState &disc) const;

private:
  const TitleIdentity &title_;
  std::optional<std::filesystem::path> file_;
};

} // namespace psx::host
