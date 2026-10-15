#pragma once

#include "disc.h"

#include <filesystem>

namespace psx::host {

struct TitleIdentity;

// A title's disc, opened for one read and closed with it.
class TitleDisc {
public:
  // The disc the title's keys, then the player's saved choice, name; opened on first read.
  explicit TitleDisc(const TitleIdentity &title);
  // Exactly `image`, opened now; opened() says whether it is a readable disc image.
  explicit TitleDisc(const std::filesystem::path &image);
  ~TitleDisc();
  TitleDisc(const TitleDisc &) = delete;
  TitleDisc &operator=(const TitleDisc &) = delete;

  // Opens a keyed disc if it is not open yet.
  bool opened();
  DiscState &state() {
    return disc_;
  }

private:
  DiscState disc_{};
};

} // namespace psx::host
