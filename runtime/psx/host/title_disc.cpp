#include "title_disc.h"

#include "saved_disc.h"

namespace psx::host {

TitleDisc::TitleDisc(const TitleIdentity &title) {
  SavedDisc(title).bind(disc_);
}

TitleDisc::TitleDisc(const std::filesystem::path &image) {
  disc_.openImage(image.c_str());
}

bool TitleDisc::opened() {
  return disc_.isOpen() || disc_.open();
}

} // namespace psx::host
